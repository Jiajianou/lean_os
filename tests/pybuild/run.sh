#!/bin/sh

echo "== m99build: what CPython's own build would cost on this machine =="

if [ ! -x /usr/bin/gcc ]; then
  echo "m99build: no native gcc on this image - skipped."
  echo "m99build: done"
  exit 0
fi

BD=/tmp/m99build
rm -rf $BD
mkdir -p $BD
cd $BD

cat > probe.c <<'PROBE'
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main (void)
{
  static int test_array [1 - 2 * !(((long int) (sizeof (int))) <= 4)];
  test_array [0] = 0;
  return test_array [0];
}
PROBE

echo "== step 1: four configure-shaped probes, compiled and linked =="
measure probe-first  gcc -o probe probe.c
measure probe-1      gcc -o probe probe.c
measure probe-2      gcc -o probe probe.c
measure probe-3      gcc -o probe probe.c

if [ -f /tests/bzip2/bzlib.c ]; then
  echo "== step 2: one C translation unit of CPython's size class =="
  measure tu gcc -Wall -O2 -D_FILE_OFFSET_BITS=64 -c /tests/bzip2/bzlib.c -o tu.o
else
  echo "m99build: no /tests/bzip2 on this image - the translation unit is skipped"
fi

echo "== step 3: the disk =="
toybox df /tmp 2>/dev/null || echo "m99build: no df"

echo "m99build: done"
