#!/bin/sh
# Runs every case in test/prove against `c-contracts prove` and diffs the
# output.
#
#   test/prove.sh <path-to-c-contracts> [filter]
#
# A case is a .c file. Its .flags file, if present, holds one INVOCATION PER
# LINE -- the extra tool arguments for that run -- so a case that only means
# something as a pair (a caller that honours a precondition and one that does
# not) stays one file. Its .expected is what the tool prints for all of them.
#
# Two things are filtered out of the comparison, both because they are true of
# the machine rather than of the code: which solver won the race, and CBMC's
# own property lines, whose indices and line numbers move with the CBMC
# version. What is left is the tool's own report.
#
# Set UPDATE=1 to rewrite the .expected files. Read the diff first.
set -u

TOOL=${1:?usage: prove.sh <path-to-c-contracts> [filter]}
FILTER=${2:-}
DIR=$(cd "$(dirname "$0")" && pwd)
INCLUDE=$DIR/../include

CFLAGS="-std=c89 -I$INCLUDE"

if ! command -v cbmc >/dev/null 2>&1; then
  # A missing prerequisite is not a pass and not a failure. Reporting FAIL here
  # would teach everyone to ignore this suite for reasons that have nothing to
  # do with the code.
  echo "SKIP: cbmc is not installed; the proof suite did not run"
  exit 0
fi

pass=0
fail=0
failed=

for case in "$DIR"/prove/*.c; do
  name=$(basename "$case" .c)
  case "$name" in
    *$FILTER*) ;;
    *) continue ;;
  esac

  flags=$DIR/prove/$name.flags
  expected=$DIR/prove/$name.expected
  fn=$(sed -n 's/^\/\* prove: \([A-Za-z_][A-Za-z0-9_]*\).*/\1/p' "$case" | head -1)
  if [ -z "$fn" ]; then
    echo "FAIL $name (no '/* prove: <function> */' line)"
    fail=$((fail + 1)); failed="$failed $name"; continue
  fi

  # One invocation per line of .flags, and one for a case that has none. Both
  # streams into one capture: prove writes stdout unbuffered precisely so that
  # a warning stays next to what it is about, and this is where that is pinned.
  { [ -f "$flags" ] && cat "$flags"; echo; } | while IFS= read -r extra; do
    [ -f "$flags" ] && [ -z "$extra" ] && continue
    # shellcheck disable=SC2086
    out=$("$TOOL" prove "$fn" "$case" $extra -- $CFLAGS 2>&1)
    printf '%s\nexit %s\n' "$out" "$?"
  done > "$DIR/prove/.$name.actual"
  actual=$(grep -v '^    \[' "$DIR/prove/.$name.actual" |
           grep -v '^solved by ' | grep -v '^$' | sed "s|$DIR/prove|CASE|g")
  rm -f "$DIR/prove/.$name.actual"

  if [ "${UPDATE:-0}" = 1 ]; then
    printf '%s\n' "$actual" > "$expected"
    echo "updated $name"
    continue
  fi

  if [ ! -f "$expected" ]; then
    echo "FAIL $name (no $name.expected; run with UPDATE=1)"
    fail=$((fail + 1)); failed="$failed $name"
  elif [ "$actual" = "$(cat "$expected")" ]; then
    pass=$((pass + 1))
  else
    echo "FAIL $name"
    printf '%s\n' "$actual" | diff -u "$expected" - | sed 's/^/    /'
    fail=$((fail + 1)); failed="$failed $name"
  fi
done

[ "${UPDATE:-0}" = 1 ] && exit 0

echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ] || { echo "failed:$failed"; exit 1; }
exit 0
