#!/bin/sh
# assert_probe harness: expects $1 (assert_fail_probe) to abort via
# assert(false). CTest cannot invert a crash with WILL_FAIL (an abort
# is reported as ***Exception failure regardless), so the inversion
# lives here: probe aborts -> pass; probe exits 0 -> assertions are
# disabled -> fail loudly.
if [ $# -ne 1 ]; then
  echo "usage: assert_probe_check.sh <assert_fail_probe binary>"
  exit 2
fi
"$1" >/dev/null 2>&1
st=$?
if [ $st -eq 0 ]; then
  echo "assert_probe: FAIL - probe exited 0, assertions are disabled (NDEBUG active?)"
  exit 1
fi
echo "assert_probe: OK - probe aborted as expected (status $st), assertions active"
exit 0
