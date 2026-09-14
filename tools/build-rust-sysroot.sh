#!/usr/bin/env bash
set -uo pipefail

cd "$(dirname "$0")/.."
ROOT=$(pwd)

TARGET=x86_64-unknown-lean_os
SPEC_DIR="$ROOT/tools/rust-port"
SOURCE="$ROOT/build/rust-src-lean_os"
SYSROOT="$ROOT/build/rust-sysroot-lean_os"
WORK="$ROOT/build/rust-lean_os"
BUILDER="$WORK/sysroot-build"
TOOLCHAIN="$ROOT/build/chromium/src/third_party/rust-toolchain"

CARGO="$TOOLCHAIN/bin/cargo"
RUSTC="$TOOLCHAIN/bin/rustc"
if [ ! -x "$CARGO" ]; then
  CARGO=$(command -v cargo 2>/dev/null)
  RUSTC=$(command -v rustc 2>/dev/null)
fi
if [ -z "$CARGO" ] || [ ! -x "$CARGO" ]; then
  echo "build-rust-sysroot: no cargo - skipped"
  exit 0
fi
if [ ! -x "$ROOT/build/toolchain/bin/x86_64-lean_os-gcc" ]; then
  echo "build-rust-sysroot: no cross toolchain - skipped"
  exit 0
fi

UPSTREAM="$TOOLCHAIN/lib/rustlib/src/rust/library"
if [ ! -d "$UPSTREAM/std" ]; then
  UPSTREAM=$("$RUSTC" --print sysroot 2>/dev/null)/lib/rustlib/src/rust/library
fi
if [ ! -d "$UPSTREAM/std" ]; then
  echo "build-rust-sysroot: no rust-src beside this rustc - skipped"
  exit 0
fi

# The fork is a copy of rust-src with tools/rust-port/apply.py's edits on it.
# The stamp is the rustc that produced the source plus every file the port is
# made of, because cargo fingerprints the tree it was handed: an edit that
# reaches this copy after a build is an edit the next build silently does not
# have. See CLAUDE.md.
STAMP="$SOURCE/.lean_os-port-stamp"
FINGERPRINT=$("$RUSTC" --version; cat "$ROOT/tools/rust-port/apply.py" \
    "$ROOT/tools/rust-port/libc/lean_os/mod.rs" \
    "$ROOT/tools/rust-port/libc/lean_os/constants.rs" \
    "$SPEC_DIR/$TARGET.json" \
    "$ROOT"/tools/rust-port/std/os/lean_os/*.rs | shasum -a 256 | cut -d' ' -f1)
if [ ! -f "$STAMP" ] || [ "$(cat "$STAMP")" != "$FINGERPRINT" ]; then
  echo "build-rust-sysroot: unpacking rust-src and applying the lean_os port"
  # Everything built from this source tree goes with it. cargo fingerprints
  # the tree it was handed, so an output directory that survives a re-unpack
  # is an output directory holding objects from the previous port - which is
  # the trap CLAUDE.md records, and it bit twice: once as "my edit did
  # nothing" and once as a linked program with no main in it.
  rm -rf "$SOURCE" "$SYSROOT" "$WORK"/*/target
  mkdir -p "$SOURCE"
  cp -a "$UPSTREAM" "$SOURCE/library"
  python3 "$ROOT/tools/rust-port/apply.py" "$SOURCE/library" || exit 1
  echo "$FINGERPRINT" > "$STAMP"
else
  python3 "$ROOT/tools/rust-port/apply.py" "$SOURCE/library" > /dev/null || exit 1
fi

# The wrapper is the whole reason this sysroot can be handed to somebody
# else's build system. rustc finds x86_64-unknown-lean_os through
# RUST_TARGET_PATH and needs -Zunstable-options to accept a target it did not
# ship with; --sysroot is added only when the caller did not pass one,
# because rustc refuses the option twice.
mkdir -p "$SYSROOT/bin" "$SYSROOT/lib/rustlib/$TARGET/lib"
cat > "$SYSROOT/bin/rustc" <<WRAPPER
#!/bin/sh
RUST_TARGET_PATH="$SPEC_DIR\${RUST_TARGET_PATH:+:\$RUST_TARGET_PATH}"
export RUST_TARGET_PATH
for argument in "\$@"; do
  case "\$argument" in
    --sysroot|--sysroot=*) exec "$RUSTC" -Zunstable-options "\$@" ;;
  esac
done
exec "$RUSTC" -Zunstable-options --sysroot "$SYSROOT" "\$@"
WRAPPER
chmod +x "$SYSROOT/bin/rustc"
for tool in rustdoc cargo; do
  ln -sf "$TOOLCHAIN/bin/$tool" "$SYSROOT/bin/$tool" 2>/dev/null || true
done

# Every target this rustc already knows stays reachable, so the host half of
# a build - build scripts, proc macros - is served by the same sysroot.
for entry in "$TOOLCHAIN"/lib/rustlib/*; do
  name=$(basename "$entry")
  [ "$name" = "$TARGET" ] && continue
  [ -e "$SYSROOT/lib/rustlib/$name" ] && continue
  ln -sf "$entry" "$SYSROOT/lib/rustlib/$name"
done

mkdir -p "$BUILDER/src"
echo '#![no_std]' > "$BUILDER/src/lib.rs"
cat > "$BUILDER/Cargo.toml" <<TOML
[package]
name = "leanos-sysroot"
version = "0.1.0"
edition = "2021"

[lib]
path = "src/lib.rs"

[profile.release]
panic = "abort"
TOML

# profiler_builtins is in Chromium's skip_stdlib_files, so its rlib has to
# exist even in a build with no instrumentation in it. Its build script
# compiles compiler-rt's profile runtime, and the Chromium checkout this
# rustc comes from carries those sources already.
# profiler_builtins compiles C against this libc's headers, so the installed
# copy has to be the tree's. `make sysroot` would delete the ported library
# stack; `make sysroot-headers` is the half that does not.
if [ -d "$ROOT/build/sysroot/usr/include" ]; then
  make -s sysroot-headers > /dev/null 2>&1 || true
fi

PROFILER_RT="$ROOT/build/chromium/src/third_party/compiler-rt/src"
if [ ! -d "$PROFILER_RT/lib/profile" ]; then
  PROFILER_RT="$ROOT/build/llvm24"
fi

echo "build-rust-sysroot: building the standard library for $TARGET"
(cd "$BUILDER" && PATH="$ROOT/build/toolchain/bin:$PATH" \
    RUSTC="$SYSROOT/bin/rustc" \
    RUST_COMPILER_RT_FOR_PROFILER="$PROFILER_RT" \
    CC_x86_64_unknown_lean_os="$ROOT/build/toolchain/bin/x86_64-lean_os-gcc" \
    AR_x86_64_unknown_lean_os="$ROOT/build/toolchain/bin/x86_64-lean_os-ar" \
    CFLAGS_x86_64_unknown_lean_os="-fno-builtin" \
    __CARGO_TESTS_ONLY_SRC_ROOT="$SOURCE/library" \
    "$CARGO" build -Z build-std=std,panic_abort,test,profiler_builtins \
    --target "$TARGET" --release) > "$ROOT/build/rust-sysroot-build.log" 2>&1 || {
  tail -30 "$ROOT/build/rust-sysroot-build.log" >&2
  echo "build-rust-sysroot: the standard library did not build" >&2
  exit 1; }

rm -f "$SYSROOT/lib/rustlib/$TARGET/lib"/*.rlib
COUNT=0
while read -r rlib; do
  cp "$rlib" "$SYSROOT/lib/rustlib/$TARGET/lib/"
  COUNT=$((COUNT + 1))
done < <(find "$BUILDER/target/$TARGET/release" -name '*.rlib')

if [ "$COUNT" -eq 0 ]; then
  echo "build-rust-sysroot: no rlibs were produced" >&2
  exit 1
fi
echo "build-rust-sysroot: $COUNT rlibs in $SYSROOT/lib/rustlib/$TARGET/lib"

REPORTED=$("$SYSROOT/bin/rustc" --print target-libdir --target "$TARGET")
if [ "$REPORTED" != "$SYSROOT/lib/rustlib/$TARGET/lib" ]; then
  echo "build-rust-sysroot: rustc reports the libdir as $REPORTED" >&2
  exit 1
fi
echo "build-rust-sysroot: rustc agrees this is where $TARGET's libraries are"
