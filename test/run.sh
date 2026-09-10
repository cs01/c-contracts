#!/bin/sh
# Every gate, in order of what it needs.
#
#   test/run.sh [gate ...]
#
# With no arguments it runs all of them. There is nothing to build first: the
# product is a header and two shell scripts, so the gates are the whole of CI.
#
# A gate whose prerequisite is missing skips loudly and does not fail: a
# contributor with only a C compiler can still run the two that matter most,
# and a skip is reported as a skip rather than counted as a pass.
set -u

DIR=$(cd "$(dirname "$0")" && pwd)
GATES=${*:-header readme prove zstd}

FAILED=
for g in $GATES; do
  [ -x "$DIR/$g.sh" ] || { echo "no such gate: $g"; exit 2; }
  "$DIR/$g.sh" || FAILED="$FAILED $g"
  echo
done

if [ -n "$FAILED" ]; then
  echo "FAILED:$FAILED"
  exit 1
fi
echo "all gates passed"
exit 0
