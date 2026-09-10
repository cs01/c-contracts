#!/bin/sh
# Race every installed CBMC solver and take the first clean answer.
#
#   ./solve.sh <goto-binary> [cbmc flags...]
#
# Solve time varies up to 20x between backends in either direction, so a static
# choice is a coin flip on a job that can run for minutes. Racing costs cores
# (cheap) instead of wall time (not).
#
# Exits with the winning cbmc's status (0 proved, 10 counterexample).
# Exits 124 if every solver hits the deadline.
set -u

TIMEOUT=${TIMEOUT:-900}
GOTO=${1:?usage: solve.sh <goto-binary> [cbmc flags...]}
shift

W=$(mktemp -d); trap 'rm -rf "$W"' EXIT

# Built-in path is bit-blast + MiniSat, named so the log says which one won.
CANDIDATES="sat:"
command -v z3       >/dev/null 2>&1 && CANDIDATES="$CANDIDATES z3:--z3"
command -v bitwuzla >/dev/null 2>&1 && CANDIDATES="$CANDIDATES bitwuzla:--bitwuzla"
command -v cvc5     >/dev/null 2>&1 && CANDIDATES="$CANDIDATES cvc5:--cvc5"

START=$(date +%s)
for C in $CANDIDATES; do
  NAME=${C%%:*}; FLAG=${C#*:}
  # shellcheck disable=SC2086
  ( cbmc "$GOTO" $FLAG "$@" > "$W/$NAME.log" 2>&1; echo $? > "$W/$NAME.rc" ) &
  echo "$!" > "$W/$NAME.pid"
done

WINNER=""; RC=124
while [ $(( $(date +%s) - START )) -lt "$TIMEOUT" ]; do
  for C in $CANDIDATES; do
    NAME=${C%%:*}
    [ -f "$W/$NAME.rc" ] || continue
    R=$(cat "$W/$NAME.rc")
    case "$R" in
      # A counterexample is definitive: the trace exists.
      10) WINNER=$NAME; RC=$R; break ;;
      # "Proved" only counts if every property was decided.
      0)  if grep -q ': UNKNOWN' "$W/$NAME.log" 2>/dev/null; then continue; fi
          WINNER=$NAME; RC=$R; break ;;
      *)  ;;
    esac
  done
  [ -n "$WINNER" ] && break
  # All solvers exited without a verdict.
  DONE=$(ls "$W"/*.rc 2>/dev/null | wc -l | tr -d ' ')
  N=$(echo "$CANDIDATES" | wc -w | tr -d ' ')
  [ "$DONE" -ge "$N" ] && break
  sleep 2
done
ELAPSED=$(( $(date +%s) - START ))

# Kill the losers.
for C in $CANDIDATES; do
  NAME=${C%%:*}
  [ "$NAME" = "$WINNER" ] && continue
  kill "$(cat "$W/$NAME.pid")" 2>/dev/null
  pkill -P "$(cat "$W/$NAME.pid")" 2>/dev/null
done

if [ -n "$WINNER" ]; then
  cat "$W/$WINNER.log"
  echo "== solved by $WINNER in ${ELAPSED}s"
  exit "$RC"
fi

echo "== no solver finished within ${TIMEOUT}s"
for C in $CANDIDATES; do
  NAME=${C%%:*}
  PHASE=$(grep -E "Starting Bounded Model Checking|converting SSA|Running|Passing problem" \
            "$W/$NAME.log" 2>/dev/null | tail -1)
  RCS=$( [ -f "$W/$NAME.rc" ] && cat "$W/$NAME.rc" || echo running )
  printf '   %-10s rc=%-8s last phase: %s\n' "$NAME" "$RCS" "${PHASE:-<none>}"
done
exit 124
