#!/usr/bin/env bash
set -uo pipefail

cd "$(dirname "$0")/.."
ROOT=$(pwd)

if command -v sha256sum >/dev/null 2>&1; then
  host_sha() { sha256sum "$1" | cut -d' ' -f1; }
elif command -v shasum >/dev/null 2>&1; then
  host_sha() { shasum -a 256 "$1" | cut -d' ' -f1; }
else
  echo "pkg-test: no sha256sum or shasum on this machine - cannot grade" >&2
  exit 1
fi

WORK=$(mktemp -d -t leanos-pkg-XXXXXX)
trap 'rm -rf "$WORK"' EXIT

make -s os-pkg >/dev/null || { echo "pkg-test: os-pkg did not build" >&2; exit 1; }
OS_PKG="$ROOT/build/os-pkg"

fail=0
note() { printf '  %s\n' "$*"; }

echo "pkg-test: this project's SHA-256 against the host's, over the tree"
n=0
mismatch=0
while IFS= read -r f; do
  rm -rf "$WORK/one"; mkdir -p "$WORK/one"
  cp "$f" "$WORK/one/f"
  printf 'name: one\nversion: 1\n' > "$WORK/one.manifest"
  "$OS_PKG" build "$WORK/one.manifest" "$WORK/one" "$WORK/one.osp" >/dev/null 2>&1 || {
    note "FAIL could not package $f"; fail=1; continue
  }
  ours=$("$OS_PKG" info "$WORK/one.osp" | awk '/ f$/ {print $(NF-1)}')
  theirs=$(host_sha "$f")
  if [ "${theirs:0:16}" != "$ours" ]; then
    note "FAIL $f: ours ${ours}, host ${theirs:0:16}"
    mismatch=$((mismatch + 1))
    fail=1
  fi
  n=$((n + 1))
done < <(find kernel user_space system_api tools tests -type f \
              \( -name '*.c' -o -name '*.h' -o -name '*.sh' \) | sort | head -250)
note "$n files hashed, $mismatch disagreements with the host"

echo "pkg-test: a package round-trips to the bytes it was built from"
STAGE="$WORK/stage"
mkdir -p "$STAGE/bin" "$STAGE/share/deep/deeper"
: > "$STAGE/share/empty"
head -c 64 /dev/urandom > "$STAGE/share/exactly-one-block"
head -c 63 /dev/urandom > "$STAGE/share/one-short"
head -c 65 /dev/urandom > "$STAGE/share/one-over"
head -c 1500000 /dev/urandom > "$STAGE/bin/big"
chmod +x "$STAGE/bin/big"
printf 'a file with a newline at the end\n' > "$STAGE/share/deep/deeper/text"
ln -s ../../bin/big "$STAGE/share/deep/link-to-big"

cat > "$WORK/stage.manifest" <<'EOF'
name: roundtrip
version: 2.0
summary: every shape the format has to carry
provides: big
caps: fs-write network
EOF

if "$OS_PKG" build "$WORK/stage.manifest" "$STAGE" "$WORK/rt.osp" >/dev/null; then
  "$OS_PKG" extract "$WORK/rt.osp" "$WORK/out" >/dev/null || fail=1
  if diff -r "$STAGE" "$WORK/out" >/dev/null 2>&1; then
    note "diff -r: the extracted tree is the staging tree"
  else
    note "FAIL the extracted tree differs from the staging tree:"
    diff -r "$STAGE" "$WORK/out" 2>&1 | head -10 | sed 's/^/    /'
    fail=1
  fi
  if cmp -s "$STAGE/bin/big" "$WORK/out/bin/big"; then
    note "cmp: the 1.5 MB payload is byte for byte"
  else
    note "FAIL the 1.5 MB payload does not compare equal"
    fail=1
  fi
else
  note "FAIL could not build the round-trip package"
  fail=1
fi

echo "pkg-test: the index's archive digest, against the host's"
mkdir -p "$WORK/repo"
cp "$WORK/rt.osp" "$WORK/repo/roundtrip-2.0.osp"
"$OS_PKG" index "$WORK/repo" >/dev/null || fail=1
indexed=$(awk '/^sha256:/ {print $2}' "$WORK/repo/index")
actual=$(host_sha "$WORK/repo/roundtrip-2.0.osp")
if [ "$indexed" = "$actual" ]; then
  note "the index and the host agree on all 64 hex digits"
else
  note "FAIL index says $indexed, the host says $actual"
  fail=1
fi

echo "pkg-test: the same tree twice produces the same archive"
"$OS_PKG" build "$WORK/stage.manifest" "$STAGE" "$WORK/rt2.osp" >/dev/null || fail=1
if cmp -s "$WORK/rt.osp" "$WORK/rt2.osp"; then
  note "two builds of one tree are the same file"
else
  note "FAIL two builds of the same tree produced different archives"
  fail=1
fi

echo "pkg-test: one changed byte in a real archive is refused"
cp "$WORK/rt.osp" "$WORK/rt-bad.osp"
size=$(wc -c < "$WORK/rt-bad.osp")
printf '\xff' | dd of="$WORK/rt-bad.osp" bs=1 seek=$((size / 2)) conv=notrunc 2>/dev/null
if "$OS_PKG" verify "$WORK/rt-bad.osp" >/dev/null 2>&1; then
  note "FAIL a corrupted archive verified"
  fail=1
else
  note "refused: $("$OS_PKG" verify "$WORK/rt-bad.osp" 2>&1 | tail -1)"
fi

if [ $fail -eq 0 ]; then
  echo "pkg-test: PASS"
else
  echo "pkg-test: FAIL"
fi
exit $fail
