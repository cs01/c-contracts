#!/bin/sh
# The header against real annotated source: zstd's decoder.
#
#   ZSTD=~/git/zstd test/zstd.sh
#
# The fixtures show that the header does what it was built to do. This shows
# that it survives a codebase that was not written for it: system headers,
# intrinsics, macro-heavy inline functions, and a translation unit nobody
# trimmed for a verifier.
#
# Each case records the status it is expected to have and the runner fails only
# on a mismatch. A missing prerequisite is a SKIP: not a pass, since nothing
# ran, and not a failure, since a contributor may reasonably not have a zstd
# checkout with the annotation branch on it.
set -u

DIR=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$DIR/.." && pwd)
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

echo "== c_contracts.h against $ZSTD =="

# ------------------------------------------------------------------ case 1
# The annotations survive a real translation unit and lower to CBMC syntax.
# The number is informational -- it tracks the zstd revision -- but a zero is
# not: it means the header expanded to nothing where it should have expanded
# to clauses.
W=$(mktemp -d); trap 'rm -rf "$W"' EXIT
# shellcheck disable=SC2086
if /usr/bin/cc -E -DC_CONTRACTS_CPROVER $CFLAGS "$TU" -o "$W/tu.i" 2>"$W/cpp.log"
then
  N=$(grep -oE '__CPROVER_(requires|ensures|assigns|loop_invariant|decreases)\b' \
        "$W/tu.i" | wc -l | tr -d ' ')
  if [ "${N:-0}" -eq 0 ]; then A=FAIL; D="no clauses lowered"
  else A=PASS; D="$N clause(s) lowered from a real translation unit"; fi
else
  A=FAIL; D=$(grep -m1 "error:" "$W/cpp.log" | cut -c1-60)
fi
report "1 lowers a real translation unit" PASS "$A" "$D"

# ------------------------------------------------------------------ case 2
# The unbounded memory-safety proof, against the contract in zstd's own source.
# No length cap: the buffers are symbolically sized, so the loop contract is
# what discharges the loop, not an unwind bound. -H because wildcopy states its
# buffers without contract_fresh; see the comment in zstd/wildcopy.c.
if ! command -v cbmc >/dev/null 2>&1; then
  report "2 ZSTD_wildcopy is memory safe, unbounded" PASS SKIP "no cbmc"
else
  T0=$(date +%s)
  # shellcheck disable=SC2086
  OUT=$(cd "$DIR/zstd" && TIMEOUT=$BUDGET "$ROOT/prove.sh" harness \
          wildcopy.c -H $CFLAGS 2>&1)
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
