#!/usr/bin/env bash
set -uo pipefail

cd "$(dirname "$0")/.."
ROOT=$(pwd)

SPEC="$ROOT/tools/rust-port/x86_64-lean_os.json"
WORK="$ROOT/build/rust-lean_os"
CRATE="$WORK/probe"
STD_CRATE="$WORK/std_probe"
SOURCE="$ROOT/build/rust-src-lean_os"
IMAGE="$ROOT/build/os-image.bin"
PREFIX="$ROOT/build/toolchain/bin/x86_64-lean_os-"
TOOLCHAIN="$ROOT/build/chromium/src/third_party/rust-toolchain"
CARGO="$TOOLCHAIN/bin/cargo"
RUSTC="$TOOLCHAIN/bin/rustc"

if [ ! -x "$CARGO" ]; then
  CARGO=$(command -v cargo 2>/dev/null)
  RUSTC=$(command -v rustc 2>/dev/null)
fi
if [ -z "$CARGO" ] || [ ! -x "$CARGO" ]; then
  echo "rust-test: no cargo - skipped"
  exit 0
fi
if [ ! -x "${PREFIX}gcc" ]; then
  echo "rust-test: no cross toolchain - skipped"
  exit 0
fi

# The two generated files are regenerated and compared rather than trusted.
# A hand edit to either is a failure, for the reason M116 learned about
# tools/iconv-test.sh: a generated file that is edited in place is a file
# that stops describing what generated it.
echo "rust-test: the generated tables still agree with what generates them"
python3 "$ROOT/tools/rust-port/gen-libc-constants.py" --check || exit 1
python3 "$ROOT/tools/rust-port/gen-abi-facts.py" --check || exit 1

echo "rust-test: the target specification loads"
"$RUSTC" -Z unstable-options --print target-spec-json --target "$SPEC" \
    > /dev/null 2>&1 || {
  echo "rust-test: $SPEC is not a target rustc accepts" >&2
  exit 1
}

mkdir -p "$CRATE/src"
cp "$ROOT/tests/rust/probe.rs" "$CRATE/src/lib.rs"
cat > "$CRATE/Cargo.toml" <<TOML
[package]
name = "probe"
version = "0.1.0"
edition = "2021"

[lib]
crate-type = ["staticlib"]

[profile.release]
panic = "abort"
TOML

echo "rust-test: building core for x86_64-lean_os"
(cd "$CRATE" && PATH="$ROOT/build/toolchain/bin:$PATH" RUSTC="$RUSTC" \
    "$CARGO" build -Z json-target-spec -Z build-std=core \
    --target "$SPEC" --release) > "$ROOT/build/rust-build.log" 2>&1 || {
  tail -20 "$ROOT/build/rust-build.log" >&2; exit 1; }

LIB="$CRATE/target/x86_64-lean_os/release/libprobe.a"
if [ ! -f "$LIB" ]; then
  echo "rust-test: $LIB was not produced" >&2
  exit 1
fi

echo "rust-test: linking the Rust staticlib with this project's C toolchain"
"${PREFIX}gcc" "$ROOT/tests/rust/main.c" "$LIB" -o "$ROOT/build/rusttest" \
    2>/dev/null || {
  echo "rust-test: the Rust objects did not link against this libc" >&2
  exit 1; }

TYPE=$("${PREFIX}readelf" -h "$ROOT/build/rusttest" | awk '/Type:/{print $2}')
if [ "$TYPE" != "EXEC" ]; then
  echo "rust-test: produced $TYPE, expected EXEC" >&2
  exit 1
fi
echo "rust-test: EXEC, $(wc -c < "$ROOT/build/rusttest" | tr -d ' ') bytes"

if [ -f "$IMAGE" ] && [ -x "$ROOT/build/leanfs-put" ]; then
  "$ROOT/build/leanfs-put" "$IMAGE" "$ROOT/build/rusttest" /bin/rusttest \
      >/dev/null || exit 1
  echo "rust-test: installed as /bin/rusttest - the [m137] boot self-test runs it"
fi

# M138: the standard library. Everything below needs rust-src, which the
# Chromium toolchain carries and a bare host rustc may not.
UPSTREAM="$TOOLCHAIN/lib/rustlib/src/rust/library"
if [ ! -d "$UPSTREAM/std" ]; then
  UPSTREAM=$("$RUSTC" --print sysroot 2>/dev/null)/lib/rustlib/src/rust/library
fi
if [ ! -d "$UPSTREAM/std" ]; then
  echo "rust-test: no rust-src beside this rustc - std half skipped"
  exit 0
fi

# The fork is a copy of rust-src with tools/rust-port/apply.py's edits on it.
# The stamp is the rustc that produced the source plus every file the port is
# made of, because a stale copy is the failure mode that costs an afternoon:
# cargo fingerprints the tree it was given, so an edit that does not reach
# this directory is an edit the build silently does not have.
STAMP="$SOURCE/.lean_os-port-stamp"
FINGERPRINT=$("$RUSTC" --version; cat "$ROOT/tools/rust-port/apply.py" \
    "$ROOT/tools/rust-port/libc/lean_os/mod.rs" \
    "$ROOT/tools/rust-port/libc/lean_os/constants.rs" \
    "$ROOT"/tools/rust-port/std/os/lean_os/*.rs | shasum -a 256 | cut -d' ' -f1)
if [ ! -f "$STAMP" ] || [ "$(cat "$STAMP")" != "$FINGERPRINT" ]; then
  echo "rust-test: unpacking rust-src and applying the lean_os port"
  rm -rf "$SOURCE" "$STD_CRATE/target"
  mkdir -p "$SOURCE"
  cp -a "$UPSTREAM" "$SOURCE/library"
  python3 "$ROOT/tools/rust-port/apply.py" "$SOURCE/library" || exit 1
  echo "$FINGERPRINT" > "$STAMP"
else
  python3 "$ROOT/tools/rust-port/apply.py" "$SOURCE/library" > /dev/null || exit 1
fi

echo "rust-test: compiling the C half of the ABI table with x86_64-lean_os-gcc"
mkdir -p "$STD_CRATE/src" "$STD_CRATE/.cargo"
"${PREFIX}gcc" -std=c11 -Wall -Wextra -Werror \
    -I "$ROOT/user_space/libc/include" -I "$ROOT/system_api/include" \
    -c "$ROOT/tests/rust/abi_facts.c" -o "$WORK/abi_facts.o" || {
  echo "rust-test: tests/rust/abi_facts.c did not compile" >&2
  exit 1; }

cp "$ROOT/tests/rust/std_probe/src/main.rs" "$STD_CRATE/src/main.rs"
cp "$ROOT/tests/rust/std_probe/src/abi_facts.rs" "$STD_CRATE/src/abi_facts.rs"
cat > "$STD_CRATE/Cargo.toml" <<TOML
[package]
name = "ruststd"
version = "0.1.0"
edition = "2021"

[dependencies]
libc = { path = "$SOURCE/library/vendor/libc-0.2.189", default-features = false }

[[bin]]
name = "ruststd"
path = "src/main.rs"

[profile.release]
panic = "abort"
TOML
cat > "$STD_CRATE/.cargo/config.toml" <<TOML
[target.x86_64-lean_os]
rustflags = ["-Clink-arg=$WORK/abi_facts.o"]
TOML

echo "rust-test: building std for x86_64-lean_os"
(cd "$STD_CRATE" && PATH="$ROOT/build/toolchain/bin:$PATH" RUSTC="$RUSTC" \
    __CARGO_TESTS_ONLY_SRC_ROOT="$SOURCE/library" \
    "$CARGO" build -Z json-target-spec -Z build-std=std,panic_abort \
    --target "$SPEC" --release) > "$ROOT/build/rust-std-build.log" 2>&1 || {
  tail -40 "$ROOT/build/rust-std-build.log" >&2; exit 1; }

BIN="$STD_CRATE/target/x86_64-lean_os/release/ruststd"
if [ ! -f "$BIN" ]; then
  echo "rust-test: $BIN was not produced" >&2
  exit 1
fi
TYPE=$("${PREFIX}readelf" -h "$BIN" | awk '/Type:/{print $2}')
if [ "$TYPE" != "EXEC" ]; then
  echo "rust-test: the std program is $TYPE, expected EXEC" >&2
  exit 1
fi
if ! "${PREFIX}nm" "$BIN" | grep -q ' T main$'; then
  echo "rust-test: the std program has no main for this crt0 to call" >&2
  exit 1
fi
cp "$BIN" "$ROOT/build/ruststd"
echo "rust-test: EXEC, $(wc -c < "$ROOT/build/ruststd" | tr -d ' ') bytes, with std in it"

if [ -f "$IMAGE" ] && [ -x "$ROOT/build/leanfs-put" ]; then
  "$ROOT/build/leanfs-put" "$IMAGE" "$ROOT/build/ruststd" /bin/ruststd \
      >/dev/null || exit 1
  echo "rust-test: installed as /bin/ruststd - the [m138] boot self-test runs it"
fi
