#!/usr/bin/env bash
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

STAGE_NAMES=()
STAGE_VERDICTS=()
STAGE_SECONDS=()
OVERALL=0

STAMPS=$(mktemp -t leanos-stamps-XXXXXX)
trap 'rm -f "$STAMPS"' EXIT

run_stage() {
  local name="$1"; shift
  local started finished verdict
  echo
  echo "=============================================================="
  echo "  $name"
  echo "=============================================================="
  started=$(date +%s)
  python3 tools/tree-stamps.py snapshot "$STAMPS"
  if "$@"; then
    verdict="pass"
  else
    verdict="FAIL"
    OVERALL=1
  fi
  if ! python3 tools/tree-stamps.py compare "$STAMPS"; then
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
  run_stage "a second build does nothing" \
    bash -c 'out=$(make all 2>&1); [ -z "$out" ] || { echo "make all ran again:"; echo "$out" | head -20; exit 1; }'
  run_stage "the ported userland, onto the image" make toybox
  if [ "$OVERALL" -ne 0 ]; then
    echo "the toybox port failed to build or install - see tools/build-toybox.sh" >&2
    exit 1
  fi
  run_stage "a program from x86_64-lean_os-gcc" ./tools/gcc-test.sh
  if [ "$OVERALL" -ne 0 ]; then
    echo "the target port produced something that will not link or run" >&2
    exit 1
  fi
  run_stage "the dynamic loader and a program that needs it" ./tools/build-dynamic.sh

  run_stage "C++, and an exception that crosses a library" ./tools/cxx-test.sh
  if [ "$OVERALL" -ne 0 ]; then
    echo "the dynamic linker or something it loads would not build" >&2
    exit 1
  fi
  run_stage "a program from x86_64-lean_os-clang, and one from both" \
    ./tools/clang-test.sh
  if [ "$OVERALL" -ne 0 ]; then
    echo "the clang port produced something that will not link or run" >&2
    exit 1
  fi
  run_stage "core, for x86_64-lean_os, linked against this libc" \
    ./tools/rust-test.sh
  if [ "$OVERALL" -ne 0 ]; then
    echo "the Rust target produced something that will not link" >&2
    exit 1
  fi

  run_stage "the native binutils, onto the image" ./tools/install-native-toolchain.sh

  run_stage "python and its library, onto the image" ./tools/install-python.sh

  run_stage "the browser, onto the image" make browser-if-built

  if [ -f build/repo/index ]; then
    run_stage "the package repository, onto the image" make packages
  else
    echo "  (no build/repo - run tools/build-packages.sh for [m111])"
  fi

  # M142 and M145 added boot markers and left their installers out of this
  # list, so `make all` wiped /bin/mathltest and /bin/chromiumbase and the
  # graded boot below reported two markers missing after every kernel change.
  # A tier that cannot go green on a clean tree is not an instrument.
  run_stage "the long double library, onto the image" \
    ./tools/math-long-double-test.sh

  run_stage "Chromium's //base, //mojo and //net, onto the image" \
    ./tools/chromium-test.sh
fi

if [ "$TIER" = "full" ]; then
  run_stage "host unit tests (including the slow ones)" \
    bash -c 'make --no-print-directory test-fast TEST_FILTER=--slow'
else
  run_stage "host unit tests" make --no-print-directory test-fast
fi

run_stage "the shell, against $(basename "${REFERENCE_SH:-/bin/sh}")" ./tools/sh-test.sh

run_stage "the regex engine, against the host's" ./tools/regex-test.sh
run_stage "scanf, against the host's" ./tools/scanf-test.sh
run_stage "libm, against the host's" ./tools/math-test.sh
run_stage "printf, against the host's" ./tools/printf-test.sh
run_stage "the FILE layer, off the machine" ./tools/stdio-test.sh
run_stage "packages, against the host's sha256 and cmp" ./tools/pkg-test.sh
run_stage "iconv, against the host's" ./tools/iconv-test.sh
run_stage "uchar, against Python's own encoders" ./tools/uchar-test.sh
run_stage "realpath, against the host's" ./tools/realpath-test.sh
run_stage "where a thread_local is, against the linker's own answer" ./tools/tls-layout-test.sh
run_stage "set-resolution's modes are the driver's" ./tools/set-resolution.sh --check

if [ "$TIER" = "full" ]; then
  run_stage "coverage, and the ratchet" make --no-print-directory coverage-check
  run_stage "mutation score (sampled)" \
    bash -c 'make --no-print-directory mutate MUTANTS=25'
  run_stage "fuzzing (60s per target)" make --no-print-directory fuzz-run
fi

if [ "$TIER" = "fast" ] || [ "$HOST_ONLY" -eq 1 ]; then
  :
else
  run_stage "boot self-tests, graded against every marker" \
    ./tools/qemu-serial-test.sh

  run_stage "four cores, and the work shared between them" ./tools/smp-test.sh

  if [ "$TIER" = "full" ]; then
    run_stage "the same battery, through the I/O APIC" \
      bash -c 'LEANOS_IOAPIC=1 ./tools/qemu-serial-test.sh 1200'
  fi

  if [ "$TIER" = "full" ]; then
    run_stage "somebody else's configure, under this shell" \
      ./tools/configure-test.sh
    run_stage "interactive suite (all tests)" ./tools/qemu-input-test.sh
    run_stage "crash consistency (16 power cuts)" ./tools/crash-test.sh
    run_stage "a disk that refuses, below the driver" ./tools/disk-fault-test.sh
    run_stage "the same, through ATA" \
      bash -c 'QEMU_DISK=ide ./tools/disk-fault-test.sh'
    run_stage "the toolchain, building somebody else's program here" \
      ./tools/bootstrap-test.sh
    run_stage "what CPython's own build would cost here" \
      ./tools/python-build-test.sh
    run_stage "the Chromium fork still fits the revision it is pinned to" \
      ./tools/chromium-test.sh
  else
    run_stage "interactive suite (--quick subset)" ./tools/qemu-input-test.sh --quick
  fi
  run_stage "a stale snapshot is refused, not used" \
    ./tools/qemu-input-test.sh --check-stale

  run_stage "the window run-qemu.sh opens is full size" ./tools/window-test.sh
fi

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
