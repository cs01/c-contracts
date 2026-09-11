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

# 2. Compile to goto program. In harness mode the entry point is named now,
# not just at solve time, so step 3 can tell which functions are reachable.
if [ "$HARNESS" = 1 ]; then
  goto-cc --function "$FN" "$W/pp.i" -o "$W/a.goto" 2>"$W/goto.log" || {
    echo "goto-cc failed:" >&2; cat "$W/goto.log" >&2; exit 3; }
else
  goto-cc "$W/pp.i" -o "$W/a.goto" 2>"$W/goto.log" || {
    echo "goto-cc failed:" >&2; cat "$W/goto.log" >&2; exit 3; }
fi

# 3. Drop what the entry point cannot reach, then apply loop contracts.
#
# The order is the whole cost of the run when the source pulls in a large
# translation unit. --apply-loop-contracts walks every loop in the program,
# so on a file that includes something like zstd's decompress unit it spends
# its time on decode loops the proof never enters -- long enough to look like
# a hang. Dropping unreachable functions first leaves only the loops the
# entry point can actually run, and the same step finishes in well under a
# second. Harmless when the program is already small.
#
# Only safe with an entry point to measure reachability from, which is why
# enforce mode skips it: there the entry point does not exist yet, it is
# generated from the contract in step 4.
if [ "$HARNESS" = 1 ]; then
  goto-instrument --drop-unused-functions "$W/a.goto" "$W/s.goto" >/dev/null 2>&1 ||
    cp "$W/a.goto" "$W/s.goto"
else
  cp "$W/a.goto" "$W/s.goto"
fi

goto-instrument --apply-loop-contracts "$W/s.goto" "$W/b.goto" >/dev/null 2>&1 ||
  cp "$W/s.goto" "$W/b.goto"

REPLACE_FLAGS=""
for R in $REPLACE; do
  REPLACE_FLAGS="$REPLACE_FLAGS --replace-call-with-contract $R"
done

if [ "$HARNESS" = 1 ]; then
  # The entry point is the one the author wrote, so there is no contract to
  # generate it from and no frame to check it against. Everything else -- the
  # loop contracts already applied above, the callees' contracts, and the
  # memory-safety checks -- still holds.
  #
  # -r still applies here, and this is where it earns its keep: replacing a
  # callee with its contract makes CBMC assert every contract_pre at the call
  # site instead of inlining the body, which is what checks that the harness
  # set up inputs the contract actually permits.
  if [ -n "$REPLACE" ]; then
    # shellcheck disable=SC2086
    goto-instrument $REPLACE_FLAGS "$W/b.goto" "$W/c.goto" >/dev/null 2>"$W/replace.log" || {
      echo "--replace-call-with-contract failed:" >&2; cat "$W/replace.log" >&2; exit 4; }
    echo "mode: harness (frame not checked), callees replaced by contract:$REPLACE"
  else
    cp "$W/b.goto" "$W/c.goto"
    echo "mode: harness (frame not checked)"
  fi
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

# 4. Prove. Solve time varies up to 20x between backends in either direction,
# so a static choice is a coin flip on a job that can run for minutes. Racing
# every installed solver and taking the first clean answer costs cores (cheap)
# instead of wall time (not). RC is cbmc's: 0 proved, 10 counterexample. 124 if
# every solver hits TIMEOUT.
S=$W/solve; mkdir -p "$S"

# Built-in path is bit-blast + MiniSat, named so the log says which one won.
CANDIDATES="sat:"
for T in z3 bitwuzla cvc5; do
  if command -v "$T" >/dev/null 2>&1; then CANDIDATES="$CANDIDATES $T:--$T"; fi
done

START=$(date +%s)
for C in $CANDIDATES; do
  NAME=${C%%:*}; FLAG=${C#*:}
  # A solver that aborts is a normal outcome of a race -- another one is still
  # running -- but the subshell would report the signal death to the terminal
  # over the top of the winner's output. cbmc's own streams are already in the
  # log, so the subshell has nothing else to say. errexit is off inside it so
  # that a counterexample (rc 10) still reaches the .rc file the race reads.
  # shellcheck disable=SC2086
  ( set +e
    cbmc "$W/c.goto" --function "$FN" $FLAG $CBMC_FLAGS > "$S/$NAME.log" 2>&1
    echo $? > "$S/$NAME.rc" ) 2>/dev/null &
  echo "$!" > "$S/$NAME.pid"
done

WINNER=""; RC=124
while [ $(( $(date +%s) - START )) -lt "${TIMEOUT:-900}" ]; do
  for C in $CANDIDATES; do
    NAME=${C%%:*}
    [ -f "$S/$NAME.rc" ] || continue
    R=$(cat "$S/$NAME.rc")
    case "$R" in
      # A counterexample is definitive: the trace exists.
      10) WINNER=$NAME; RC=$R; break ;;
      # "Proved" only counts if every property was decided.
      0)  if grep -q ': UNKNOWN' "$S/$NAME.log" 2>/dev/null; then continue; fi
          WINNER=$NAME; RC=$R; break ;;
      *)  ;;
    esac
  done
  if [ -n "$WINNER" ]; then break; fi
  # All solvers exited without a verdict.
  DONE=$(ls "$S"/*.rc 2>/dev/null | wc -l | tr -d ' ')
  N=$(echo "$CANDIDATES" | wc -w | tr -d ' ')
  if [ "$DONE" -ge "$N" ]; then break; fi
  sleep 2
done
ELAPSED=$(( $(date +%s) - START ))

# Kill the losers.
for C in $CANDIDATES; do
  NAME=${C%%:*}
  if [ "$NAME" = "$WINNER" ]; then continue; fi
  kill "$(cat "$S/$NAME.pid")" 2>/dev/null || true
  pkill -P "$(cat "$S/$NAME.pid")" 2>/dev/null || true
done

if [ -n "$WINNER" ]; then
  cat "$S/$WINNER.log"
  echo "== solved by $WINNER in ${ELAPSED}s"
else
  echo "== no solver finished within ${TIMEOUT:-900}s"
  for C in $CANDIDATES; do
    NAME=${C%%:*}
    PHASE=$(grep -E "Starting Bounded Model Checking|converting SSA|Running|Passing problem" \
              "$S/$NAME.log" 2>/dev/null | tail -1)
    RCS=$( [ -f "$S/$NAME.rc" ] && cat "$S/$NAME.rc" || echo running )
    printf '   %-10s rc=%-8s last phase: %s\n' "$NAME" "$RCS" "${PHASE:-<none>}"
  done
  exit 124
fi

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
