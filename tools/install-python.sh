#!/usr/bin/env bash
# tools/install-python.sh - M99: put CPython on the disk.
#
# The install half of tools/build-python.sh, in the same shape as
# tools/install-native-toolchain.sh and for the same reasons: it skips
# with a message when the build has not happened (an image without
# Python is a valid image), it strips on the way in, and it puts the
# standard library on the filesystem as **.py source files** rather than
# freezing anything into the binary.
#
# That last part is M99's second bullet and it is worth stating plainly:
# M80 froze the library into the executable because M81's filesystem
# could not hold two thousand small files. M93's can - it is the
# milestone whose whole subject was a source tree - so the library goes
# on the disk the way it does everywhere else, and `import` reads it
# from there. An OS whose Python cannot see a .py file cannot run
# anybody's script.
set -uo pipefail

cd "$(dirname "$0")/.."
ROOT=$(pwd)

PYBUILD=build/python/build
IMAGE="${LEANOS_IMAGE:-build/os-image.bin}"
PUT=build/leanfs-put
CROSS_STRIP=build/toolchain/bin/x86_64-lean_os-strip
PY_VER=3.12.7
PY_MAJMIN=3.12

if [ ! -x "$PYBUILD/python" ]; then
  echo "install-python: no python at $PYBUILD/python - skipped."
  echo "          tools/build-python.sh builds it (M99); it is not part of \`make\`."
  exit 0
fi
[ -f "$IMAGE" ] || { echo "install-python: no image at $IMAGE - run make first" >&2; exit 1; }
[ -x "$PUT" ] || make -s leanfs-put || exit 1

STAGE=build/python/install
rm -rf "$STAGE"
mkdir -p "$STAGE"

cp "$PYBUILD/python" "$STAGE/python3" || exit 1
"$CROSS_STRIP" "$STAGE/python3" || exit 1
"$PUT" "$IMAGE" "$STAGE/python3" /bin/python3 >/dev/null || exit 1
"$PUT" -s "$IMAGE" /bin/python3 /bin/python >/dev/null 2>&1 || true

# The standard library. Everything CPython's own `make install` would
# put in $prefix/lib/python3.12, minus the parts that are meaningless
# here (the test suite is installed separately below, on purpose, so
# that an image can carry the library without carrying 40 MB of tests).
SRC="build/toolchain-src/Python-$PY_VER"
LIBSTAGE=build/python/lib
rm -rf "$LIBSTAGE"
mkdir -p "$LIBSTAGE"
( cd "$SRC/Lib" && tar cf - --exclude=test --exclude=idlelib --exclude=tkinter \
    --exclude=turtledemo --exclude='__pycache__' . ) | ( cd "$LIBSTAGE" && tar xf - )

"$PUT" -r "$IMAGE" "$LIBSTAGE" /usr/lib/python$PY_MAJMIN >/dev/null || exit 1

COUNT=$(find "$LIBSTAGE" -name '*.py' | wc -l | tr -d ' ')
SIZE=$(du -sh "$LIBSTAGE" | cut -f1)
echo "install-python: python3 in /bin, and $COUNT .py files ($SIZE) under /usr/lib/python$PY_MAJMIN"

# The regression suite, which is M99's fourth bullet and the most
# valuable thing in this milestone: a test suite somebody else wrote,
# reporting its own failures. Installed under the library where
# `python3 -m test` expects it.
if [ "${LEANOS_PYTHON_TESTS:-1}" = "1" ]; then
  TESTSTAGE=build/python/tests
  rm -rf "$TESTSTAGE"
  mkdir -p "$TESTSTAGE"
  ( cd "$SRC/Lib/test" && tar cf - --exclude='__pycache__' . ) | \
    ( cd "$TESTSTAGE" && tar xf - )
  "$PUT" -r "$IMAGE" "$TESTSTAGE" /usr/lib/python$PY_MAJMIN/test >/dev/null || exit 1
  echo "install-python: and CPython's own regression suite under .../test"
fi

# The fixtures the [m99] boot self-test runs - M80's bar and the bar it
# said mattered more.
"$PUT" -r "$IMAGE" tests/python /tests/python >/dev/null || exit 1
