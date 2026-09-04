#!/bin/sh
# tests/pybuild/run.sh - M99's last box, measured ON the machine.
#
# What CPython's own `./configure && make` would cost here. Run by the
# kernel when the boot was given `opt/leanos/pybuild=1` through fw_cfg;
# tools/python-build-test.sh is what passes it and what grades what comes
# back. Each line is printed as it is produced, for the reason M98's own
# fixture gives: a measurement that exists only if the whole run finishes
# is the one you lose exactly when you most want it.
#
# ---- why this measures units and multiplies ----------------------------
#
# M98 timed a build that FITS - bzip2, 203 s - and then graded every
# object byte for byte. That is the better instrument and it is not
# available here. CPython is ~450 translation units plus a configure that
# runs 753 compile-and-link probes, and M99's own box already did the
# arithmetic that says this does not fit. **M98's rule is that when a
# build does not fit, the number that says by how much is the
# deliverable** - so the job is to make that number measured rather than
# guessed.
#
# Two units are measured here, on this machine, with this machine's own
# compiler:
#
#   probe   what one configure check costs: a few lines of C with the
#           headers autoconf includes, compiled AND LINKED. The link is
#           roughly half of it and is the half a "how fast is the
#           compiler" figure leaves out. Four of them, and the first is
#           reported separately because it is DIFFERENT - see below.
#   tu      one real C translation unit of CPython's size class,
#           compiled. Not linked: the Makefile links twice, not 450
#           times.
#
# The counts are facts about CPython measured off the machine and
# recorded in tools/python-build-test.sh, which does the multiplication
# and prints the arithmetic. Nothing here estimates anything.

echo "== m99build: what CPython's own build would cost on this machine =="

# /usr/bin, not /bin: tools/install-native-toolchain.sh puts the native
# toolchain under /usr, and PATH_DEFAULT is "/bin:/usr/bin" so `gcc`
# finds it either way. Testing the wrong path here skipped the whole
# measurement and said so, which is the failure mode a guard is supposed
# to have and is still a guard about the wrong file.
if [ ! -x /usr/bin/gcc ]; then
  echo "m99build: no native gcc on this image - skipped."
  echo "m99build: done"
  exit 0
fi

BD=/tmp/m99build
rm -rf $BD
mkdir -p $BD
cd $BD

# ---- unit one: a configure probe ---------------------------------------
#
# This is autoconf's own AC_CHECK_SIZEOF body, not an invented one: the
# array-size trick is how every `checking size of ...` line in a
# 753-check run is compiled, and the three headers are the ones
# confdefs.h has pulled in by the time it gets there.
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

# The first run is labelled `first` and not `cold`, and the difference
# is a measurement rather than a naming preference. The obvious guess is
# that the first is the expensive one because it demand-pages cc1; the
# machine says the opposite - the first is 4,530 ms and the three after
# it are 7,330 - and the split says why: USER time is the same (~80 cs)
# and SYSTEM time doubles (142 cs to 283). What changes after the first
# run is that there is now a previous `probe` on the disk to overwrite
# and dirty blocks to write back, and this compiler is not what this
# machine is waiting for.
echo "== step 1: four configure-shaped probes, compiled and linked =="
measure probe-first  gcc -o probe probe.c
measure probe-1      gcc -o probe probe.c
measure probe-2      gcc -o probe probe.c
measure probe-3      gcc -o probe probe.c

# ---- unit two: a translation unit of CPython's size class ---------------
#
# bzip2's bzlib.c, which is already on this disk for M98's bootstrap and
# is 1,572 lines - CPython's mean C file is about 1,900. Using a file
# that is already here rather than putting CPython's 40 MB source tree on
# the image is the whole reason this measurement is minutes instead of a
# day, and the size class is stated so the extrapolation can be argued
# with rather than believed.
if [ -f /tests/bzip2/bzlib.c ]; then
  echo "== step 2: one C translation unit of CPython's size class =="
  measure tu gcc -Wall -O2 -D_FILE_OFFSET_BITS=64 -c /tests/bzip2/bzlib.c -o tu.o
else
  echo "m99build: no /tests/bzip2 on this image - the translation unit is skipped"
fi

# ---- and what the source tree itself would cost ------------------------
#
# The other half of "does it fit", and the half that is about the disk
# rather than the clock. Reported as what is free, because a number of
# bytes with nothing to compare it to is not an answer.
echo "== step 3: the disk =="
toybox df /tmp 2>/dev/null || echo "m99build: no df"

echo "m99build: done"
