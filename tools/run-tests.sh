#!/usr/bin/env bash
# tools/run-tests.sh - Q1: the one command that tests lean_os.
#
# ---- Why this exists --------------------------------------------------
#
# There were two harnesses and no way to run them. `tools/run-qemu.sh`
# built and booted the machine, and because the kernel ran every one of
# its self-tests on every boot, booting *was* testing - which meant the
# two were the same thing and neither was a command. Whether a commit had
# been checked was a question about what a person remembered to type.
#
# So the tests moved here and the boot stayed there. Three tiers, and the
# tier is chosen by how long you are willing to wait:
#
#   --fast    host unit tests only. No QEMU, no cross-toolchain, under a
#             second. This is the one that runs while you type.
#   (default) the fast tier, plus a full boot graded against every marker,
#             plus the quick interactive subset. Minutes. Run before a
#             commit.
#   --full    everything: the slow host tests that exhaust real resources,
#             the whole interactive suite, coverage and its ratchet, a
#             sampled mutation score, a short fuzzing run, and sixteen
#             power cuts to check the filesystem survives them. Tens of
#             minutes. Run before a milestone, and nightly.
#
# Each tier is a superset of the one above it. A tier that passes never
# means a broader one would - it means the checks in it passed, which is
# the honest claim and the only one worth making.
#
# Usage:
#   tools/run-tests.sh              # the pre-commit tier
#   tools/run-tests.sh --fast       # host units only
#   tools/run-tests.sh --full       # everything
#   tools/run-tests.sh --host-only  # skip anything needing QEMU
#   tools/run-tests.sh --no-build   # grade what is already built
set -uo pipefail

cd "$(dirname "$0")/.."

TIER="commit"
DO_BUILD=1
HOST_ONLY=0

while [ $# -gt 0 ]; do
  case "$1" in
    --fast)      TIER="fast"; shift ;;
    --full)      TIER="full"; shift ;;
    --host-only) HOST_ONLY=1; shift ;;
    --no-build)  DO_BUILD=0; shift ;;
    -h|--help)   sed -n '2,30p' "$0"; exit 0 ;;
    *) echo "unknown argument: $1 (try --help)" >&2; exit 2 ;;
  esac
done

# ---- Reporting --------------------------------------------------------
#
# Every stage records its own name, verdict and wall-clock, and the
# summary prints all of them. A run that says "3 of 4 stages passed, and
# here is which one did not and how long each took" is a report; "FAILED"
# is a starting point for an investigation.
STAGE_NAMES=()
STAGE_VERDICTS=()
STAGE_SECONDS=()
OVERALL=0

run_stage() {
  local name="$1"; shift
  local started finished verdict
  echo
  echo "=============================================================="
  echo "  $name"
  echo "=============================================================="
  started=$(date +%s)
  if "$@"; then
    verdict="pass"
  else
    verdict="FAIL"
    OVERALL=1
  fi
  finished=$(date +%s)
  STAGE_NAMES+=("$name")
  STAGE_VERDICTS+=("$verdict")
  STAGE_SECONDS+=("$(( finished - started ))")
  echo "-- $name: $verdict ($(( finished - started ))s)"
}

if [ "$DO_BUILD" -eq 1 ] && [ "$TIER" != "fast" ] && [ "$HOST_ONLY" -eq 0 ]; then
  run_stage "build the image" make all
  if [ "$OVERALL" -ne 0 ]; then
    echo "build failed - nothing below would mean anything" >&2
    exit 1
  fi
  # M89: and the ported userland, onto that image.
  #
  # `make toybox` is not part of `all` - see the Makefile for the inode
  # ordering that keeps it out - but the graded boot below has a marker
  # for it, and a tier that skipped the port would be grading an image
  # that is missing the thing the milestone added. So it is a stage of
  # its own here: visible in the summary, timed like every other, and
  # failing loudly rather than turning the boot's M89 check into a skip.
  run_stage "the ported userland, onto the image" make toybox
  if [ "$OVERALL" -ne 0 ]; then
    echo "the toybox port failed to build or install - see tools/build-toybox.sh" >&2
    exit 1
  fi
  # M94: and the program this project's own compiler produces, if that
  # compiler has been built. Skips with a message otherwise - see
  # tools/gcc-test.sh, which is also where the compile line lives.
  run_stage "a program from x86_64-lean_os-gcc" ./tools/gcc-test.sh
  if [ "$OVERALL" -ne 0 ]; then
    echo "the target port produced something that will not link or run" >&2
    exit 1
  fi
  # M95: the dynamic linker, the shared libc, and the two fixtures. Same
  # shape and the same reason as the stage above - it needs the compiler
  # M94 built, and skips with a message when that is not there.
  run_stage "the dynamic loader and a program that needs it" ./tools/build-dynamic.sh

  # M97: after build-dynamic.sh, and the order is not arbitrary - the
  # cross-object C++ fixtures need ld-lean.so and libc.so on the image
  # before an executable whose PT_INTERP names the loader can start at
  # all, and a program that cannot start reports "command not found",
  # which is a message about the shell.
  run_stage "C++, and an exception that crosses a library" ./tools/cxx-test.sh
  if [ "$OVERALL" -ne 0 ]; then
    echo "the dynamic linker or something it loads would not build" >&2
    exit 1
  fi
fi

# ---- Stage 1: the host tier ------------------------------------------
if [ "$TIER" = "full" ]; then
  # --slow includes the tests that exhaust the inode table and the data
  # region. They are the most valuable tests in the suite and they take
  # minutes, which is exactly why they are here and not in every tier.
  run_stage "host unit tests (including the slow ones)" \
    bash -c 'make --no-print-directory test-fast TEST_FILTER=--slow'
else
  run_stage "host unit tests" make --no-print-directory test-fast
fi

# ---- M86: the shell, against a shell nobody here wrote ---------------
#
# In every tier including --fast, because it costs about a second and is
# the only instrument that grades /bin/sh against an implementation this
# project did not write. See tests/sh/README.md for why a shell in
# particular needs that and a boot marker will not do.
run_stage "the shell, against $(basename "${REFERENCE_SH:-/bin/sh}")" ./tools/sh-test.sh

# ---- M89: the regex engine, against an engine nobody here wrote ------
#
# The same instrument and the same argument, applied to the other place
# in this tree where "correct" means "agrees with everyone else". A
# regular expression engine that decided its own spans would be wrong in
# a way no self-written test could see - so the host's own <regex.h>
# decides, and tests/regex/cases.tsv says nothing about what the answers
# should be. In every tier for the same reason as the shell: about a
# second, and it grades something a boot marker cannot.
run_stage "the regex engine, against the host's" ./tools/regex-test.sh
# M89: the third differential test, for the same reason as the other two -
# see tools/scanf-test.sh. In the fast tier, next to them, because it is
# a second and costs nothing.
run_stage "scanf, against the host's" ./tools/scanf-test.sh

# ---- Q11/Q12: the instruments that grade the tests themselves --------
#
# Only in --full. Both are minutes rather than seconds, and both answer a
# question about the suite rather than about the machine - which is worth
# asking regularly and not before every commit.
if [ "$TIER" = "full" ]; then
  run_stage "coverage, and the ratchet" make --no-print-directory coverage-check
  run_stage "mutation score (sampled)" \
    bash -c 'make --no-print-directory mutate MUTANTS=25'
  run_stage "fuzzing (60s per target)" make --no-print-directory fuzz-run
fi

if [ "$TIER" = "fast" ] || [ "$HOST_ONLY" -eq 1 ]; then
  :
else
  # ---- Stage 2: the boot, graded --------------------------------------
  #
  # This is where the kernel's own self-tests run. They are off on an
  # ordinary boot as of Q1 (see kernel/dev/fwcfg.h); qemu-serial-test.sh
  # is what turns them on.
  run_stage "boot self-tests, graded against every marker" \
    ./tools/qemu-serial-test.sh

  # M106: four cores. A separate harness rather than a flag on the one
  # above, and tools/smp-test.sh's own header says why at length - the
  # short version is that this kernel had never started a second core in
  # a test, and what that hid was five real bugs. In the default tier
  # because it is a two-minute boot and it is the only thing standing
  # between the multi-core path and rotting again the same way.
  run_stage "four cores, and the work shared between them" ./tools/smp-test.sh

  # M103: and the same battery again with every legacy line routed
  # through the I/O APIC instead of the 8259. In `--full` only, because
  # it is a second ~250-second boot and because the default path is the
  # one this machine actually runs on - see kernel/dev/fwcfg.h for the
  # measurement that decided which is which.
  if [ "$TIER" = "full" ]; then
    run_stage "the same battery, through the I/O APIC" \
      bash -c 'LEANOS_IOAPIC=1 ./tools/qemu-serial-test.sh 500'
  fi

  # ---- Stage 3: real clicks on real pixels ----------------------------
  if [ "$TIER" = "full" ]; then
    run_stage "interactive suite (all tests)" ./tools/qemu-input-test.sh
    # Q17: the one guarantee the filesystem makes, tested by taking the
    # power away rather than by reading the code that provides it.
    run_stage "crash consistency (16 power cuts)" ./tools/crash-test.sh
    # Q16: and a disk that refuses, from BELOW the driver. The [q16] boot
    # marker injects at the block layer, which grades the filesystem and
    # the recovery; this grades the two drivers themselves, because a
    # fault above ata_read_sectors never reaches the ERR bit it polls
    # for. Both backends, because they fail in completely different
    # places - a status byte and a status register.
    run_stage "a disk that refuses, below the driver" ./tools/disk-fault-test.sh
    run_stage "the same, through ATA" \
      bash -c 'QEMU_DISK=ide ./tools/disk-fault-test.sh'
  else
    run_stage "interactive suite (--quick subset)" ./tools/qemu-input-test.sh --quick
  fi
fi

# ---- Summary ----------------------------------------------------------
echo
echo "=============================================================="
echo "  summary - $TIER tier"
echo "=============================================================="
total=0
for i in "${!STAGE_NAMES[@]}"; do
  printf '  %-6s %5ss  %s\n' "${STAGE_VERDICTS[$i]}" "${STAGE_SECONDS[$i]}" "${STAGE_NAMES[$i]}"
  total=$(( total + STAGE_SECONDS[i] ))
done
printf '  %-6s %5ss  total\n' "" "$total"

# Q6: the run itself, recorded. A single verdict says whether today is
# broken; a file of them says when it broke and how the cost is trending.
mkdir -p build
if [ ! -f build/test-history.tsv ]; then
  printf 'when\tcommit\tharness\tverdict\twall_s\tboot_s\n' > build/test-history.tsv
fi
printf '%s\t%s\t%s\t%s\t%s\t%s\n' \
  "$(date -u +%Y-%m-%dT%H:%M:%SZ)" \
  "$(git rev-parse --short HEAD 2>/dev/null || echo unknown)" \
  "run-tests:$TIER" \
  "$([ "$OVERALL" -eq 0 ] && echo pass || echo fail)" \
  "$total" "" >> build/test-history.tsv

echo
if [ "$OVERALL" -eq 0 ]; then
  echo "PASS - every stage in the $TIER tier passed."
else
  echo "FAIL - see the stage(s) marked FAIL above."
fi
exit "$OVERALL"
