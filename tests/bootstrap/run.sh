#!/bin/sh
# tests/bootstrap/run.sh - M98's fourth box, run ON the machine.
#
# Everything below runs on lean_os, in lean_os's own shell, using this
# machine's own compiler, assembler, linker, archiver and make. The
# kernel spawns it when the boot was given `opt/leanos/bootstrap=1`
# through fw_cfg (tools/bootstrap-test.sh is what passes it) and waits
# for it; everything printed here reaches the serial line as it happens,
# because a spawned process's stdout on this machine IS the kernel log
# (syscall.c's sys_write, FD_STDOUT).
#
# **That is a deliberate change from the first version**, which collected
# everything into a file for the kernel to print at the end. The second
# run took longer than its ceiling, the harness stopped the machine, and
# the file - with every measurement in it - went with it. A measurement
# that exists only if the whole run finishes is the one you lose exactly
# when you most want it, so each line is printed as it is produced and a
# run that is cut short still says how far it got.
#
# ---- what it measures, and why each one is here -----------------------
#
#   cxx-tu      A header- and template-heavy C++ translation unit, which
#               is what M98's "peak RSS of the largest translation unit"
#               is actually asking about: GCC is a C++ program and its
#               own worst files are C++ files. This is the number M102's
#               swap decision waits on.
#   c-tu        The same question for C, for the comparison.
#   bzip2       Somebody else's build system, run by this machine's make
#               and this machine's compiler: seven translation units, an
#               archive built with ar and indexed with ranlib, two linked
#               programs, and then bzip2's OWN test suite - which
#               compresses reference files and compares them byte for
#               byte against reference output nobody here produced. A
#               build that ends by grading itself is worth more than a
#               build that merely ends.
#   -j1 vs -j4  The same build twice. The second is the process- and
#               descriptor-pressure measurement M98's third box asks for,
#               and the kernel's own peak counters are what report it.
#   identity    Every object this machine compiled, compared byte for
#               byte with the object the CROSS compiler produced from the
#               same source with the same flags. Same GCC version, same
#               port, same target - so the bytes should be equal, and if
#               they are not, the difference is a fact about this
#               machine's libc or its assembler rather than an opinion.
#               This is the strongest bootstrap-shaped statement that
#               fits here: a full three-stage GCC bootstrap does not, and
#               the milestone says the honest outcome then is the number
#               that says by how much.
#   disk        What the build tree costs, and what the toolchain costs,
#               which is the other half of the fourth box.
BS=/tmp/bs

echo "== m98boot: a build on this machine =="

rm -rf $BS
mkdir -p $BS
cd $BS

# ---- step 0, and the reason it is first --------------------------------
#
# Two lines of C++ that reach `.cfi_personality`, which is the directive
# the machine's own `as` rejected the first time this machine compiled
# C++ for itself - twenty-eight of them, in a fifteen-minute compile
# that then failed in the assembler. A reproduction that costs a second
# belongs in front of one that costs fifteen minutes, and the line it
# prints is the evidence: "bad expression" was a message about what gas
# READ, and what gas read was `%#x` - the three characters a printf with
# no `#` flag writes where a number belongs.
echo "== step 0: the assembler, on a directive C++ needs =="
cp /tests/bootstrap/cfi.cc cfi.cc
g++ -O2 -S cfi.cc -o cfi.s
echo "-- what the compiler wrote:"
toybox grep -n "cfi_personality" cfi.s
echo "-- what the assembler makes of it:"
as cfi.s -o cfi.o
echo "-- as exit: $?"

echo "== step 1: a C++ translation unit through cc1plus =="
cp /tests/bootstrap/bigtu.cc bigtu.cc
measure cxx-tu g++ -O2 -c bigtu.cc -o bigtu.o

echo "== step 2: somebody else's source tree =="
# toybox's cp, by its full name: /bin/cp is this project's own two
# argument copy (M89's collision rule), and a recursive copy is what a
# multi-call binary is reached by name for.
toybox cp -r /tests/bzip2 bzip2
cd $BS/bzip2

# CFLAGS is passed rather than left to bzip2's own default, and the
# reason is the identity check below: the default carries `-g`, and a
# `-g` object records the directory it was compiled in - so two builds of
# the same source in two different directories are correctly different
# and the comparison would measure nothing but the path. Without it the
# only file name in the object is the one on the command line, which is
# the same on both sides. The host-side reference objects are built with
# this exact line (tools/install-native-toolchain.sh).
CF="-Wall -Winline -O2 -D_FILE_OFFSET_BITS=64"

echo "== step 3: one C translation unit =="
measure c-tu gcc $CF -c bzlib.c -o /tmp/bs/c-tu.o

echo "== step 4: bzip2, serially =="
measure bzip2-j1 make -j1 CFLAGS="$CF" libbz2.a bzip2 bzip2recover

echo "== step 5: bzip2's own test suite, on this machine =="
measure bzip2-test make CFLAGS="$CF" test

# The reference objects were produced host-side by the cross compiler
# from these same files with these same flags (see
# tools/install-native-toolchain.sh). Reported per file: a difference is
# a finding, not a failure of this script.
#
# One line per object rather than a running total: this shell has no
# arithmetic expansion (see user_space/shell/sh.c, which says so and says
# why), and a counter kept with `expr` would be three processes per file
# to avoid printing eight lines.
echo "== step 6: objects, against the cross compiler's =="
for o in blocksort.o huffman.o crctable.o randtable.o compress.o decompress.o bzlib.o bzip2.o; do
    if cmp -s $o /tests/bzip2-ref/$o; then
        echo "identical: $o"
    else
        echo "differs: $o"
    fi
done

echo "== step 7: disk =="
toybox du -sk $BS
toybox du -sk /usr
toybox du -sk /bin
toybox df -k /

# LAST, and the ordering is a lesson rather than a preference. This step
# is the only one whose result is a number rather than a fact, and it is
# also the slowest: four concurrent compiles of ~120 MiB each, on one
# core, cost far more than four times one of them - which is itself the
# process-pressure measurement M98's third box asks for. The first run
# that put it in the middle spent its whole ceiling here and lost the
# disk figures that come free, so everything cheap now happens first.
#
# ---- M100: and it is bounded now, because it does not finish ----------
#
# Putting it last was not enough. `make -j4` here has never returned:
# M98's own table in the archive records it as "did not finish inside a
# 40-minute ceiling", and two runs at a 3,600 s ceiling in M100 both sat
# in this step with no serial output for the better part of an hour while
# `-j1` compiled the same eight files in 188 s. Unbounded, it took the
# whole harness with it - so the three [perf] rows the kernel prints
# after this script never appeared and tools/bootstrap-test.sh has
# **never passed**, reporting the situation as four missing lines.
#
# `measure -t` bounds the step and reports it as what it is: a command
# still running at its limit, exit 124, wall-clock deliberately not
# emitted as a budget row because the number would be the limit rather
# than the cost. Everything after this line then runs, which is where
# build_wall_s and the two peak counters come from.
#
# 900 s is chosen from a measurement rather than an opinion: `-j1` is
# 188 s, and four jobs on one core that took more than five times one
# job's wall clock would already be the answer M98's third box is
# asking for. It is a *diagnosis*, not a deadline to grow when it
# fires - the day this step finishes, that fact is the news.
echo "== step 8: bzip2 again, four jobs at once =="
make clean
measure -t 900 bzip2-j4 make -j4 CFLAGS="$CF" libbz2.a bzip2 bzip2recover

echo "== m98boot done =="
