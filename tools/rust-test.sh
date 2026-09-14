#!/usr/bin/env bash
set -uo pipefail

cd "$(dirname "$0")/.."
ROOT=$(pwd)

SPEC="$ROOT/tools/rust-port/x86_64-lean_os.json"
WORK="$ROOT/build/rust-lean_os"
CRATE="$WORK/probe"
IMAGE="$ROOT/build/os-image.bin"
PREFIX="$ROOT/build/toolchain/bin/x86_64-lean_os-"
CARGO="$ROOT/build/chromium/src/third_party/rust-toolchain/bin/cargo"

if [ ! -x "$CARGO" ]; then
  CARGO=$(command -v cargo 2>/dev/null)
fi
if [ -z "$CARGO" ] || [ ! -x "$CARGO" ]; then
  echo "rust-test: no cargo - skipped"
  exit 0
fi
if [ ! -x "${PREFIX}gcc" ]; then
  echo "rust-test: no cross toolchain - skipped"
  exit 0
fi

echo "rust-test: the target specification loads"
"$ROOT/build/chromium/src/third_party/rust-toolchain/bin/rustc" \
    -Z unstable-options --print target-spec-json --target "$SPEC" \
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
(cd "$CRATE" && PATH="$ROOT/build/toolchain/bin:$PATH" \
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
