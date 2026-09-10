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
# -H       harness mode: <function> is an entry point you wrote, not a contract
#          to enforce. Memory-safety checks and loop contracts still apply, and
#          the contracts of everything it calls are still checked; the named
#          function's own frame is not, because it has no contract to check it
#          against. Needed when a callee states its buffers with
#          contract_readable / contract_writable and no contract_fresh: those
#          say the memory is accessible but not which object it belongs to, so
#          there is nothing for a generated entry point to allocate.
#
# Needs: a C preprocessor (cc), goto-cc, goto-instrument, cbmc (all CBMC 6+).
set -eu

FN=${1:?usage: prove.sh <function> <source.c> [-I dir ...] [-- cbmc flags]}
SRC=${2:?usage: prove.sh <function> <source.c> [-I dir ...] [-- cbmc flags]}
shift 2

CFLAGS=""; CBMC_FLAGS=""; REPLACE=""; HARNESS=0
while [ $# -gt 0 ]; do
  case "$1" in
    -r) REPLACE="$REPLACE $2"; shift 2 ;;
    -H) HARNESS=1; shift ;;
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

# Count clauses, not lines that hold one: the preprocessor puts a whole
# declaration on one line, so `grep -c` reports 1 for a function with six.
N=$(grep -oE '__CPROVER_(requires|ensures|assigns|loop_invariant|decreases)\b' \
      "$W/pp.i" 2>/dev/null | wc -l | tr -d ' ')
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

if [ "$HARNESS" = 1 ]; then
  # The entry point is the one the author wrote, so there is no contract to
  # generate it from and no frame to check it against. Everything else -- the
  # loop contracts already applied above, the callees' contracts, and the
  # memory-safety checks -- still holds.
  cp "$W/b.goto" "$W/c.goto"
  echo "mode: harness (frame not checked)"
else
  # A loop without a contract does not make goto-instrument decline politely:
  # it aborts, and the shell then prints "Aborted (core dumped)" over the top
  # of the message that would actually help. The trailing `exit` is
  # load-bearing: a subshell whose only command is the tool gets exec'd into
  # it, dies by signal and gets reported anyway. With a second statement the
  # subshell stays a shell, exits normally, and the diagnostic below is the
  # only thing the user sees.
  # shellcheck disable=SC2086
  ( goto-instrument --enforce-contract "$FN" $REPLACE_FLAGS \
      "$W/b.goto" "$W/c.goto" >/dev/null; exit $? ) 2>"$W/enforce.log" || {
    if grep -qi "loops remain" "$W/enforce.log"; then
      echo "$FN has loops without contracts:" >&2
      echo "  add contract_assigns / contract_invariant / contract_decreases to each loop" >&2
    elif grep -qi "not found" "$W/enforce.log"; then
      echo "$FN not found in $SRC" >&2
    elif grep -qi "no definite size for lvalue target" "$W/enforce.log"; then
      echo "$FN's frame cannot be sized from its contract." >&2
      echo "  A buffer stated with contract_readable / contract_writable and no" >&2
      echo "  contract_fresh has no object behind it. Add contract_fresh, or" >&2
      echo "  write an entry point and pass -H." >&2
    else
      cat "$W/enforce.log" >&2
    fi
    exit 4; }
  echo "mode: enforce (frame checked)"
fi

# 4. Prove.
HERE=$(cd "$(dirname "$0")" && pwd)
if [ -x "$HERE/solve.sh" ]; then
  # shellcheck disable=SC2086
  "$HERE/solve.sh" "$W/c.goto" --function "$FN" $CBMC_FLAGS
else
  # shellcheck disable=SC2086
  cbmc "$W/c.goto" --function "$FN" $CBMC_FLAGS
fi
RC=$?

# 5. Vacuity, and only on success. Preconditions nothing can satisfy leave the
# body unreachable, so every property holds and the proof proves nothing --
# the one failure that looks exactly like success and stays that way forever.
# A proof that FAILED already reached the body, so there is nothing to ask.
#
# The enforce harness assumes the preconditions before calling, so asking
# whether any block of the original body is coverable asks exactly that. In
# harness mode there is no generated entry point and no preconditions to be
# unsatisfiable, so there is nothing to ask.
[ "$RC" -eq 0 ] || exit "$RC"
[ "$HARNESS" = 0 ] || exit 0
if ! cbmc "$W/c.goto" --function "$FN" --cover location 2>/dev/null |
     grep -q "__CPROVER_contracts_original_$FN\.coverage\..*SATISFIED"; then
  echo "error: $FN's preconditions are unsatisfiable -- nothing can call it," >&2
  echo "       so a proof about it proves nothing" >&2
  exit 5
fi
exit 0
