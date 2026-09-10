#!/bin/sh
# Prove a function's contract with CBMC.
#
#   ./prove.sh <function> <harness.c> [-I dir ...] [-- cbmc flags]
#
# The harness includes the source and calls the function under test.
# Contracts written with c_contracts.h are lowered to CBMC syntax by
# preprocessing with -DC_CONTRACTS_CPROVER.
#
# Needs: a C preprocessor (cc), goto-cc, goto-instrument, cbmc (all from CBMC 6+).
# Does not need clang, LLVM, or the c-contracts binary.
set -eu

FN=${1:?usage: prove.sh <function> <harness.c> [-I dir ...] [-- cbmc flags]}
HARNESS=${2:?usage: prove.sh <function> <harness.c> [-I dir ...] [-- cbmc flags]}
shift 2

CFLAGS=""; CBMC_FLAGS=""
while [ $# -gt 0 ]; do
  case "$1" in
    --) shift; CBMC_FLAGS="$*"; break ;;
    *)  CFLAGS="$CFLAGS $1"; shift ;;
  esac
done
[ -n "$CBMC_FLAGS" ] || CBMC_FLAGS="--pointer-overflow-check --bounds-check --pointer-check"

W=$(mktemp -d); trap 'rm -rf "$W"' EXIT

# 1. Preprocess: contract macros become CBMC builtins.
# shellcheck disable=SC2086
cc -E -DC_CONTRACTS_CPROVER ${CPPFLAGS:-} $CFLAGS "$HARNESS" -o "$W/pp.i" 2>"$W/cpp.log" || {
  echo "preprocessing $HARNESS failed:" >&2
  grep -m5 "error:" "$W/cpp.log" >&2; exit 2; }

N=$(grep -c "__CPROVER_requires\|__CPROVER_ensures\|__CPROVER_assigns" "$W/pp.i" 2>/dev/null || true)
echo "lowered ${N:-0} contract clause(s)"

# 2. Compile to goto program.
goto-cc "$W/pp.i" -o "$W/a.goto" 2>"$W/goto.log" || {
  echo "goto-cc failed:" >&2; cat "$W/goto.log" >&2; exit 3; }

# 3. Apply loop contracts (invariants, decreases). Safe to skip if none present.
goto-instrument --apply-loop-contracts "$W/a.goto" "$W/b.goto" >/dev/null 2>&1 ||
  cp "$W/a.goto" "$W/b.goto"

# 4. Enforce the function's contract (checks the assigns frame).
# If the function has no contract or goto-instrument cannot find it, fall
# through without --enforce-contract so cbmc still runs the harness.
ENFORCED=0
if goto-instrument --enforce-contract "$FN" "$W/b.goto" "$W/c.goto" 2>"$W/enforce.log"; then
  ENFORCED=1
else
  cp "$W/b.goto" "$W/c.goto"
  if grep -qi "loops remain" "$W/enforce.log"; then
    echo "warning: $FN has loops without contracts; frame not checked" >&2
    echo "  add contract_assigns / contract_invariant / contract_decreases to each loop" >&2
  fi
fi
[ "$ENFORCED" = 1 ] && echo "mode: enforce (frame checked)" || echo "mode: harness only (no frame check)"

# 5. Prove.
HERE=$(cd "$(dirname "$0")" && pwd)
if [ -x "$HERE/solve.sh" ]; then
  # shellcheck disable=SC2086
  exec "$HERE/solve.sh" "$W/c.goto" --function harness $CBMC_FLAGS
else
  # shellcheck disable=SC2086
  exec cbmc "$W/c.goto" --function harness $CBMC_FLAGS
fi
