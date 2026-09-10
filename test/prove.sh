#!/bin/sh
# Runs every case in test/prove through ../prove.sh and diffs the output.
#
#   test/prove.sh [filter]
#
# A case is a .c file. Every `/* prove: <fn> [args] */` line in it is one
# invocation of prove.sh, so a case that only means something as a pair -- a
# caller that honours a precondition and one that does not -- stays one file.
# Its .expected is what prove.sh prints for all of them, in order.
#
# The comparison keeps a whitelist, not everything minus a blacklist: CBMC
# prints its version, its phases, its property lines and whatever library
# warnings the build happens to emit, and all of that moves with the CBMC
# version rather than with this repo. What is kept is prove.sh's own report,
# its diagnostics, the verdict and the exit status.
#
# Set UPDATE=1 to rewrite the .expected files. Read the diff first.
set -u

FILTER=${1:-}
DIR=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$DIR/.." && pwd)
INCLUDE=$ROOT/include

if ! command -v cbmc >/dev/null 2>&1; then
  # A missing prerequisite is not a pass and not a failure. Reporting FAIL here
  # would teach everyone to ignore this suite for reasons that have nothing to
  # do with the code.
  echo "SKIP: cbmc is not installed; the proof suite did not run"
  exit 0
fi

echo "== prove.sh over test/prove =="

pass=0
fail=0
failed=

for case in "$DIR"/prove/*.c; do
  name=$(basename "$case" .c)
  case "$name" in
    *$FILTER*) ;;
    *) continue ;;
  esac

  expected=$DIR/prove/$name.expected
  actual=

  # One invocation per `prove:` line. Said out loud rather than skipped
  # silently, so a case that simply lost its header is still visible.
  invocations=$(sed -n 's|^/\* prove: \(.*\) \*/$|\1|p' "$case")
  if [ -z "$invocations" ]; then
    echo "     $name (no prove: line, not a case)"
    continue
  fi

  # Both streams into one capture: prove.sh writes its report to stdout and its
  # diagnostics to stderr, and a diagnostic belongs next to what it is about.
  actual=$(printf '%s\n' "$invocations" | while IFS= read -r inv; do
    [ -n "$inv" ] || continue
    # The first word is the function; anything after it is extra prove.sh
    # arguments for this invocation.
    fn=${inv%% *}
    rest=${inv#"$fn"}
    # Run from the case directory so the paths in diagnostics are the bare
    # file name and do not carry whoever's checkout this is.
    # shellcheck disable=SC2086
    out=$(cd "$DIR/prove" && "$ROOT/prove.sh" "$fn" "$name.c" $rest \
            -I "$INCLUDE" 2>&1)
    printf '%s\nexit %s\n' "$out" "$?"
  done | grep -E '^lowered |^mode: |^error: |^ +so a proof|^VERIFICATION |'\
'has loops without contracts|^  add contract_|^ +not found|^exit ')

  if [ "${UPDATE:-0}" = 1 ]; then
    printf '%s\n' "$actual" > "$expected"
    echo "  updated $name"
    continue
  fi

  if [ ! -f "$expected" ]; then
    echo "  FAIL $name (no $name.expected; run with UPDATE=1)"
    fail=$((fail + 1)); failed="$failed $name"
  elif [ "$actual" = "$(cat "$expected")" ]; then
    printf '  %-20s ok\n' "$name"
    pass=$((pass + 1))
  else
    echo "  FAIL $name"
    printf '%s\n' "$actual" | diff -u "$expected" - | sed 's/^/    /'
    fail=$((fail + 1)); failed="$failed $name"
  fi
done

[ "${UPDATE:-0}" = 1 ] && exit 0

echo
echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ] || { echo "failed:$failed"; exit 1; }
exit 0
