#!/bin/sh
# Prove a function's contract with CBMC.
#
#   ./prove.sh <function> <source.c> [-I dir ...] [-r dep ...] [-- cbmc flags]
#
# No harness needed. The contract annotations are the spec: prove.sh
# preprocesses them to CBMC syntax, and --enforce-contract generates
# the entry point from preconditions automatically.
#
# -r dep   modular verification: trust dep's contract instead of inlining it.
#          Use after proving dep separately. Repeat for multiple dependencies.
#
# Needs: a C preprocessor (cc), goto-cc, goto-instrument, cbmc (all CBMC 6+).
set -eu

FN=${1:?usage: prove.sh <function> <source.c> [-I dir ...] [-- cbmc flags]}
SRC=${2:?usage: prove.sh <function> <source.c> [-I dir ...] [-- cbmc flags]}
shift 2

CFLAGS=""; CBMC_FLAGS=""; REPLACE=""
while [ $# -gt 0 ]; do
  case "$1" in
    -r) REPLACE="$REPLACE $2"; shift 2 ;;
    --) shift; CBMC_FLAGS="$*"; break ;;
    *)  CFLAGS="$CFLAGS $1"; shift ;;
  esac
done
[ -n "$CBMC_FLAGS" ] || CBMC_FLAGS="--pointer-overflow-check --bounds-check --pointer-check --malloc-may-fail"

W=$(mktemp -d); trap 'rm -rf "$W"' EXIT

# 1. Preprocess: contract macros become CBMC builtins.
# Disable platform intrinsics that goto-cc cannot parse (ARM NEON, SVE, etc.).
PLATFORM_FLAGS=""
case "$(uname -m)" in
  arm64|aarch64) PLATFORM_FLAGS="-U__ARM_NEON -U__ARM_FEATURE_SVE -U__ARM_FEATURE_SVE2" ;;
esac
# shellcheck disable=SC2086
/usr/bin/cc -E -DC_CONTRACTS_CPROVER $PLATFORM_FLAGS ${CPPFLAGS:-} $CFLAGS "$SRC" -o "$W/pp.i" 2>"$W/cpp.log" || {
  echo "preprocessing $SRC failed:" >&2
  grep -m5 "error:" "$W/cpp.log" >&2; exit 2; }

N=$(grep -c "__CPROVER_requires\|__CPROVER_ensures\|__CPROVER_assigns" "$W/pp.i" 2>/dev/null || true)
[ "${N:-0}" -gt 0 ] || { echo "no contract clauses found on $FN" >&2; exit 2; }
echo "lowered ${N} clause(s)"

# 2. Compile to goto program.
goto-cc "$W/pp.i" -o "$W/a.goto" 2>"$W/goto.log" || {
  echo "goto-cc failed:" >&2; cat "$W/goto.log" >&2; exit 3; }

# 3. Apply loop contracts (invariants, decreases), then enforce the function contract.
goto-instrument --apply-loop-contracts "$W/a.goto" "$W/b.goto" >/dev/null 2>&1 ||
  cp "$W/a.goto" "$W/b.goto"

REPLACE_FLAGS=""
for R in $REPLACE; do
  REPLACE_FLAGS="$REPLACE_FLAGS --replace-call-with-contract $R"
done

# shellcheck disable=SC2086
goto-instrument --enforce-contract "$FN" $REPLACE_FLAGS "$W/b.goto" "$W/c.goto" 2>"$W/enforce.log" || {
  if grep -qi "loops remain" "$W/enforce.log"; then
    echo "$FN has loops without contracts:" >&2
    echo "  add contract_assigns / contract_invariant / contract_decreases to each loop" >&2
  elif grep -qi "not found" "$W/enforce.log"; then
    echo "$FN not found in $SRC" >&2
  else
    cat "$W/enforce.log" >&2
  fi
  exit 4; }
echo "mode: enforce (frame checked)"

# 4. Prove.
HERE=$(cd "$(dirname "$0")" && pwd)
if [ -x "$HERE/solve.sh" ]; then
  # shellcheck disable=SC2086
  exec "$HERE/solve.sh" "$W/c.goto" --function "$FN" $CBMC_FLAGS
else
  # shellcheck disable=SC2086
  exec cbmc "$W/c.goto" --function "$FN" $CBMC_FLAGS
fi
