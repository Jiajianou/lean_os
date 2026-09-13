#!/usr/bin/env bash
set -uo pipefail

cd "$(dirname "$0")/.."
ROOT=$(pwd)

PYBUILD=build/python/build
IMAGE="${LEANOS_IMAGE:-build/os-image.bin}"
PUT=build/leanfs-put
CROSS_STRIP=build/toolchain/bin/x86_64-lean_os-strip
PY_VER=3.12.7
PY_MAJMIN=3.12

pymake_var() {
  ( cd "$PYBUILD" 2>/dev/null || exit 0
    printf 'include Makefile\n__leanos_print:\n\t@echo $(%s)\n' "$1" \
      > .leanos-print.mk
    make -s -f .leanos-print.mk __leanos_print 2>/dev/null
    rm -f .leanos-print.mk )
}

BUILDEXE=$(sed -n 's/^BUILDEXE=[[:space:]]*//p' "$PYBUILD/Makefile" 2>/dev/null)
PYBIN="$PYBUILD/python$BUILDEXE"

if [ ! -x "$PYBIN" ]; then
  echo "install-python: no python at $PYBIN - skipped."
  echo "          tools/build-python.sh builds it (M99); it is not part of \`make\`."
  exit 0
fi
[ -f "$IMAGE" ] || { echo "install-python: no image at $IMAGE - run make first" >&2; exit 1; }
[ -x "$PUT" ] || make -s leanfs-put || exit 1

STAGE=build/python/install
rm -rf "$STAGE"
mkdir -p "$STAGE"

cp "$PYBIN" "$STAGE/python3" || exit 1
"$CROSS_STRIP" "$STAGE/python3" || exit 1
"$PUT" "$IMAGE" "$STAGE/python3" /bin/python3 >/dev/null || exit 1
"$PUT" -s "$IMAGE" /bin/python3 /bin/python >/dev/null 2>&1 || true

SONAME=$(pymake_var INSTSONAME)
if [ -n "$SONAME" ] && [ -f "$PYBUILD/$SONAME" ]; then
  cp "$PYBUILD/$SONAME" "$STAGE/$SONAME" || exit 1
  "$CROSS_STRIP" "$STAGE/$SONAME" || exit 1
  "$PUT" "$IMAGE" "$STAGE/$SONAME" "/lib/$SONAME" >/dev/null || exit 1
  echo "install-python: /lib/$SONAME ($(du -h "$STAGE/$SONAME" | cut -f1))"
fi

SRC="build/toolchain-src/Python-$PY_VER"
LIBSTAGE=build/python/lib
rm -rf "$LIBSTAGE"
mkdir -p "$LIBSTAGE"
( cd "$SRC/Lib" && tar cf - --exclude=test --exclude=idlelib --exclude=tkinter \
    --exclude=turtledemo --exclude='__pycache__' . ) | ( cd "$LIBSTAGE" && tar xf - )

SYSCONFIGDATA=$(find "$PYBUILD/build" -name '_sysconfigdata_*.py' | head -1)
[ -n "$SYSCONFIGDATA" ] || {
  echo "install-python: no _sysconfigdata_*.py under $PYBUILD/build - the build did not finish" >&2
  exit 1
}
cp "$SYSCONFIGDATA" "$LIBSTAGE/" || exit 1

mkdir -p "$LIBSTAGE/lib-dynload"
SHAREDMODS=$(pymake_var SHAREDMODS)
NMODS=0
for m in $SHAREDMODS; do
  if [ ! -f "$PYBUILD/$m" ]; then
    echo "install-python: $m was configured but not built" >&2
    exit 1
  fi
  cp "$PYBUILD/$m" "$LIBSTAGE/lib-dynload/" || exit 1
  "$CROSS_STRIP" "$LIBSTAGE/lib-dynload/$(basename "$m")" || exit 1
  NMODS=$((NMODS + 1))
done
[ "$NMODS" = 0 ] || echo "install-python: $NMODS extension modules as shared objects in lib-dynload"

"$PUT" -r "$IMAGE" "$LIBSTAGE" /usr/lib/python$PY_MAJMIN >/dev/null || exit 1

COUNT=$(find "$LIBSTAGE" -name '*.py' | wc -l | tr -d ' ')
SIZE=$(du -sh "$LIBSTAGE" | cut -f1)
echo "install-python: python3 in /bin, and $COUNT .py files ($SIZE) under /usr/lib/python$PY_MAJMIN"

if [ "${LEANOS_PYTHON_TESTS:-1}" = "1" ]; then
  TESTSTAGE=build/python/tests
  rm -rf "$TESTSTAGE"
  mkdir -p "$TESTSTAGE"
  ( cd "$SRC/Lib/test" && tar cf - --exclude='__pycache__' . ) | \
    ( cd "$TESTSTAGE" && tar xf - )
  "$PUT" -r "$IMAGE" "$TESTSTAGE" /usr/lib/python$PY_MAJMIN/test >/dev/null || exit 1
  echo "install-python: and CPython's own regression suite under .../test"
fi

"$PUT" -r "$IMAGE" tests/python /tests/python >/dev/null || exit 1
