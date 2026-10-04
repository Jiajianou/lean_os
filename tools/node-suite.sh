#!/usr/bin/env bash
set -uo pipefail

# M224. Node's own regression tests on this machine - the instrument that
# neither wrote its own assertions nor chose what to assert, as CPython's
# suite was in M99. Hours under TCG for all of test/parallel, so it is not a
# tier stage; it is run by hand and reports Node's own counts.
#
#   tools/node-suite.sh                      # all of test/parallel
#   tools/node-suite.sh parallel test-fs-    # only names containing test-fs-
#
# The tree goes onto the image under /lib/node/test, the runner is
# tests/node/suite.js, and the machine runs it through opt/leanos/node and
# switches itself off.

cd "$(dirname "$0")/.."

SUITES="${1:-parallel}"
FILTER="${2:--}"
PER_TEST="${LEANOS_NODE_TEST_TIMEOUT:-120}"
IMAGE="${LEANOS_IMAGE:-build/os-image.bin}"
TESTS=build/chromium/src/third_party/electron_node/test

[ -d "$TESTS" ] || { echo "node-suite: no $TESTS - run tools/fetch-electron.sh" >&2; exit 1; }
# The tests run ls, grep, cat and sh, and name /usr/bin/env by that path, so
# the userland goes on first - a bare make leaves an image with none of it.
# First, because the toybox step can recreate the image, and node with it.
make -s toybox > /dev/null || { echo "node-suite: make toybox failed" >&2; exit 1; }
./tools/node-test.sh > /dev/null || { echo "node-suite: tools/node-test.sh failed" >&2; exit 1; }
make -s leanfs-put > /dev/null 2>&1
for part in common fixtures ${SUITES//,/ }; do
  build/leanfs-put -r "$IMAGE" "$TESTS/$part" "/lib/node/test/$part" > /dev/null || exit 1
done

LEANOS_NODE_LOG=build/node-suite-serial.log \
LEANOS_NODE_ARGS="/lib/node/test $SUITES $FILTER $PER_TEST" \
  ./tools/node-boot.sh /lib/node-test/suite.js "${LEANOS_NODE_SUITE_SECONDS:-21600}" |
  grep -aE '^\[suite\]' | tail -n "${LEANOS_NODE_SUITE_TAIL:-40}"
grep -aE '^\[suite\] [0-9]+ tests in' build/node-suite-serial.log
