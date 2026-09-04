#!/bin/sh
# tests/python/run.sh - M99's fourth bullet, run ON the machine.
#
# CPython's own regression suite, one module at a time, on lean_os. The
# kernel spawns this when the boot was given `opt/leanos/pytest=1`
# through fw_cfg (tools/python-test.sh is what passes it) and waits for
# it; everything printed here reaches the serial line as it happens,
# because a spawned process's stdout on this machine IS the kernel log.
#
# ---- why this is the most valuable test in the milestone --------------
#
# Nothing in it was written here. The assertions are CPython's, the
# selection of what to assert is CPython's, and what counts as a pass is
# CPython's. Every other instrument in this project was written by the
# same hands as the thing it grades - which is why tools/sh-test.sh,
# regex-test.sh, scanf-test.sh, printf-test.sh and math-test.sh all
# reach outside for an oracle. This reaches outside for the whole test.
#
# ---- one module per `python3 -m test` run, and why --------------------
#
# Not one run with a list. A module that hangs, crashes the interpreter,
# or takes the harness past its ceiling costs only itself this way, and
# the modules already run are already printed. That is the same lesson
# tests/bootstrap/run.sh's header records at length, applied before it
# had to be learned twice.
#
# ---- the list, and why each one is on it ------------------------------
#
# Chosen so that every module grades something this OS had to grow, and
# so that the whole run fits in a boot. Wall-clock here is roughly two
# orders of magnitude above the host (M98 measured 128x for a C compile),
# so a suite that takes a minute there takes an hour here and a list
# that "covers Python" would not finish.
#
#   test_math, test_float, test_cmath   the libm M99 wrote seven
#                                       functions for, graded by
#                                       somebody else's tolerances
#   test_int, test_dict, test_list,     the interpreter itself, which is
#   test_exceptions, test_types         what says the compiler that
#                                       built it did not miscompile it
#   test_os, test_stat, test_posixpath  the syscalls: stat, listdir,
#                                       unlink, mkdir, rename, and the
#                                       path semantics on top of them
#   test_io, test_fileio                the FILE layer M98 rebuilt and
#                                       M99 taught to set errno
#   test_time                           the clock, and strftime
#   test_json, test_re                  two standard-library modules
#                                       read off this disk as .py files
#   test_subprocess                     fork, exec, wait and pipes
#                                       (M83, M84, M86) through
#                                       somebody else's process layer
#   test_threading                      M79's threads and M96's futex,
#                                       under a GIL that uses both
set -u

MODULES="test_math test_float test_cmath test_int test_dict test_list
         test_exceptions test_types test_stat test_posixpath test_os
         test_fileio test_io test_time test_json test_re test_subprocess
         test_threading"

echo "== m99pytest: CPython's own regression suite, on this machine =="
/bin/python3 -c "import sys; print('m99pytest: interpreter', sys.version)"

cd /tmp
for t in $MODULES; do
    echo "== m99pytest module: $t =="
    # Three separate ways of not running forever, and they bound
    # different things:
    #
    #   -u          so the suite's own output is not sitting in a buffer
    #               when a module is stopped part-way through.
    #   --timeout   CPython's own per-TEST watchdog. It turns one hung
    #               test into a reported failure with a traceback.
    #   timeout(1)  a wall-clock bound on the MODULE, which the line
    #               above is not: a module of two thousand tests that
    #               are each merely slow never trips a per-test
    #               watchdog. Without this one module can eat the
    #               harness's whole ceiling and the seventeen after it
    #               are never reached - which is exactly what the first
    #               run of this did. toybox's timeout, because /bin/sh
    #               has no job control to build one out of.
    #
    # The wall-clock bound is calibrated to THIS machine rather than
    # copied from a desktop. M98 measured a C compile here at 128x the
    # host's. The first run of this fixture bounded a module at 300 s and
    # reported test_math as a hang; it was not one - the watchdog fired
    # inside testPerm, which computes several thousand factorials of
    # integers up to 500 in arbitrary precision. A ceiling that turns
    # "slow" into "hung" reports the wrong thing.
    #
    # ---- and why CPython's own --timeout is NOT passed ----------------
    #
    # Because it costs `subprocess`, and that is a much bigger loss than
    # a per-test watchdog is a gain.
    #
    # `--timeout` makes libregrtest call faulthandler.dump_traceback_later,
    # which starts a WATCHDOG THREAD. From that moment the interpreter is
    # a threaded process - and **fork is refused from a threaded process
    # on this machine**, deliberately, since M83: making a page
    # copy-on-write clears the writable bit in one CPU's page tables and
    # this kernel has no TLB shootdown to tell the others, so a second
    # thread on another core would go on writing to a page the child was
    # just promised is its own. M83 chose a clean refusal over a page
    # that is sometimes shared, and that is still the right call.
    #
    # The visible symptom was three steps removed from any of that:
    # `SystemError: <built-in function fork_exec> returned NULL without
    # setting an exception`, from CPython's _posixsubprocess, whose last
    # two lines are "set an exception if errno is non-zero" and "return
    # NULL if the pid is -1" - and a fork that failed with errno 0 is the
    # one input that makes those disagree. fork() sets EAGAIN now (M99),
    # so the failure at least names itself; not passing --timeout is what
    # stops it happening at all.
    toybox timeout 1800 /bin/python3 -u -m test $t
    echo "== m99pytest exit $t: $? =="
done

echo "== m99pytest done =="
