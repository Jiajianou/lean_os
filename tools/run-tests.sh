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

  # ---- Stage 3: real clicks on real pixels ----------------------------
  if [ "$TIER" = "full" ]; then
    run_stage "interactive suite (all tests)" ./tools/qemu-input-test.sh
    # Q17: the one guarantee the filesystem makes, tested by taking the
    # power away rather than by reading the code that provides it.
    run_stage "crash consistency (16 power cuts)" ./tools/crash-test.sh
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
