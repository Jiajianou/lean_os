#!/usr/bin/env bash
# tools/pkg-test.sh - M111: the package format, graded by something that
# is not this project.
#
# ---- why this exists beside tests/test_ospkg.c -------------------------
#
# That file grades every refusal, thoroughly, and it grades them against
# expectations this project wrote down. This grades the two things it
# cannot:
#
#   1. **The hash is SHA-256.** Not "a hash that is deterministic" and
#      not "a hash this implementation agrees with itself about" - the
#      actual function, decided by the host's own `shasum -a 256`, over
#      several hundred real files from this tree. tests/test_sha256.c
#      uses the standard's vectors, which is the other half of this and
#      is a fixture; this one runs over whatever happens to be in the
#      repository, which is a corpus nobody chose.
#
#   2. **A package round-trips to the bytes it was made from.** The
#      archive is extracted by the reader and compared file by file with
#      `cmp` against the staging directory it was built from. `cmp`
#      decides, not this project.
#
# This is the fifth instrument of the kind M99's math-test.sh describes:
# "nothing in those fixtures says what the right answer is - a program
# nobody here wrote decides". It costs about a second and is in --fast.
#
# Usage: tools/pkg-test.sh
set -uo pipefail

cd "$(dirname "$0")/.."
ROOT=$(pwd)

# The host's SHA-256, under whichever name this machine has it. Both are
# checked because macOS ships `shasum` and Linux ships `sha256sum`, and a
# test that silently skipped on one of them would be a test that runs
# nowhere in particular.
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

# ---- 1. the hash ------------------------------------------------------
#
# Every source file in the tree, up to a few hundred, hashed both ways.
# Source files rather than a generated corpus on purpose: they span
# every length from a one-line header to a 12,000-line syscall.c, which
# is exactly the block-boundary coverage a hand-written corpus has to
# remember to include.
echo "pkg-test: this project's SHA-256 against the host's, over the tree"
n=0
mismatch=0
while IFS= read -r f; do
  # os-pkg has no "hash one file" mode and does not need one: a
  # one-file package's single record IS the hash of that file, printed
  # by `info`. Using the real path through the format rather than a
  # side door means this grades what a package actually carries.
  rm -rf "$WORK/one"; mkdir -p "$WORK/one"
  cp "$f" "$WORK/one/f"
  printf 'name: one\nversion: 1\n' > "$WORK/one.manifest"
  "$OS_PKG" build "$WORK/one.manifest" "$WORK/one" "$WORK/one.osp" >/dev/null 2>&1 || {
    note "FAIL could not package $f"; fail=1; continue
  }
  ours=$("$OS_PKG" info "$WORK/one.osp" | awk '/ f$/ {print $(NF-1)}')
  theirs=$(host_sha "$f")
  # os-pkg's listing prints the first sixteen characters of the digest,
  # which is what a person reads; the comparison uses that prefix. Two
  # different files agreeing on 64 bits of SHA-256 is not a thing that
  # happens by accident, and the full digest is compared in section 2
  # below through the archive hash.
  if [ "${theirs:0:16}" != "$ours" ]; then
    note "FAIL $f: ours ${ours}, host ${theirs:0:16}"
    mismatch=$((mismatch + 1))
    fail=1
  fi
  n=$((n + 1))
done < <(find kernel user_space system_api tools tests -type f \
              \( -name '*.c' -o -name '*.h' -o -name '*.sh' \) | sort | head -250)
note "$n files hashed, $mismatch disagreements with the host"

# ---- 2. the round trip ------------------------------------------------
#
# A staging tree with the shapes that matter - an empty file, a file
# whose length is exactly a hash block, a nested directory, a symlink,
# and something megabyte-sized - packaged, extracted, and compared with
# `cmp` and `diff -r`.
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
  # And byte for byte on the big one, which `diff -r` on a binary would
  # report as "differ" without saying where.
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

# ---- 3. the archive hash, in full -------------------------------------
#
# The index records a SHA-256 over the whole .osp file, and `os install`
# refuses an archive that does not match it. That digest is this
# project's; this checks it against the host's, at full length.
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

# ---- 4. determinism ---------------------------------------------------
#
# The same tree must produce the same archive, byte for byte. Without
# this, "did this rebuild produce the same package" has no answer and
# nothing about a package can be compared across builds.
echo "pkg-test: the same tree twice produces the same archive"
"$OS_PKG" build "$WORK/stage.manifest" "$STAGE" "$WORK/rt2.osp" >/dev/null || fail=1
if cmp -s "$WORK/rt.osp" "$WORK/rt2.osp"; then
  note "two builds of one tree are the same file"
else
  note "FAIL two builds of the same tree produced different archives"
  fail=1
fi

# ---- 5. a refusal, end to end -----------------------------------------
#
# The host tests cover every refusal in the reader. This checks that one
# of them survives the whole tool: a single byte changed in the middle of
# a real archive must make `os-pkg verify` exit nonzero and say why.
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
