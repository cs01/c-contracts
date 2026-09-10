#!/bin/sh
# The tool against real annotated source: zstd's decoder.
#
#   ZSTD=~/git/zstd test/zstd.sh <path-to-c-contracts>
#
# Fixtures show that the tool does what it was built to do. This shows that it
# survives a codebase that was not written for it: system headers, intrinsics,
# macro-heavy inline functions, and a CFG with edges clang records but builds no
# block for -- which crashed the call-site pass the first time it was pointed
# here.
#
# Each case records the status it is expected to have and the runner fails only
# on a mismatch. A missing prerequisite is a SKIP: not a pass, since nothing
# ran, and not a failure, since a contributor may reasonably not have a zstd
# checkout with the annotation branch on it.
set -u

TOOL=${1:?usage: zstd.sh <path-to-c-contracts>}
# Absolute: case 2 runs from test/zstd, so that the proof directory beside the
# case is the one --proof-dir defaults to.
TOOL=$(cd "$(dirname "$TOOL")" && pwd)/$(basename "$TOOL")
DIR=$(cd "$(dirname "$0")" && pwd)
ZSTD=${ZSTD:-$HOME/git/zstd}
BUDGET=${BUDGET:-60}
FAILED=0
SKIPPED=0

report() { # name expected actual detail
  if [ "$3" = "SKIP" ]; then
    printf '  %-44s %-6s %s\n' "$1" SKIP "$4"; SKIPPED=$((SKIPPED + 1))
  elif [ "$2" = "$3" ]; then
    printf '  %-44s %-6s %s\n' "$1" "$3" "$4"
  else
    printf '  %-44s %-6s %s  <-- RECORDED %s\n' "$1" "$3" "$4" "$2"
    FAILED=$((FAILED + 1))
  fi
}

TU=$ZSTD/lib/decompress/zstd_decompress_block.c
if [ ! -f "$TU" ]; then
  echo "SKIP: no zstd checkout at \$ZSTD ($ZSTD)"
  exit 0
fi
if ! grep -qs "c_contracts.h" "$ZSTD/lib/common/zstd_internal.h"; then
  echo "SKIP: $ZSTD is not on a branch that carries the annotations"
  exit 0
fi

CFLAGS="-DNDEBUG -U__ARM_NEON -DZSTD_NO_INTRINSICS -I $ZSTD/lib/common -I $ZSTD/lib"

echo "== c-contracts against $ZSTD =="

# ------------------------------------------------------------------ case 1
# Levels 1 and 2 over a real translation unit. The number is informational --
# it tracks the zstd revision -- but a crash or a zero is not.
# shellcheck disable=SC2086
N=$("$TOOL" --list "$TU" -- $CFLAGS 2>/dev/null | grep -c "condition of")
RC=$?
if [ "$RC" -ge 2 ]; then A=FAIL; D="the tool exited $RC"
elif [ "${N:-0}" -eq 0 ]; then A=FAIL; D="no clauses found"
else A=PASS; D="$N clauses read out of a stock parse"; fi
report "1 reads a real translation unit" PASS "$A" "$D"

# ------------------------------------------------------------------ case 2
# The unbounded memory-safety proof, from the annotations in zstd's own source
# and a released clang. No length cap: the buffers are symbolically sized, so
# the loop contracts are what discharge the loops, not an unwind bound.
if ! command -v cbmc >/dev/null 2>&1; then
  report "2 ZSTD_wildcopy is memory safe, unbounded" PASS SKIP "no cbmc"
else
  T0=$(date +%s)
  # shellcheck disable=SC2086
  OUT=$(cd "$DIR/zstd" && "$TOOL" prove ZSTD_wildcopy wildcopy.c \
          --timeout="$BUDGET" -- $CFLAGS 2>&1)
  E=$(( $(date +%s) - T0 ))
  if printf '%s' "$OUT" | grep -q "VERIFICATION SUCCESSFUL"; then
    if [ "$E" -le "$BUDGET" ]; then A=PASS; D="${E}s, budget ${BUDGET}s"
    else A=FAIL; D="${E}s, over the ${BUDGET}s budget"; fi
  else
    A=FAIL; D=$(printf '%s' "$OUT" | grep -m1 "FAILURE\|error:" | cut -c1-60)
  fi
  report "2 ZSTD_wildcopy is memory safe, unbounded" PASS "$A" "$D"
fi

echo
[ "$SKIPPED" -gt 0 ] && echo "$SKIPPED case(s) skipped for a missing prerequisite, not a result"
if [ "$FAILED" -eq 0 ]; then echo "every case that ran behaved as recorded"; fi
exit "$FAILED"
