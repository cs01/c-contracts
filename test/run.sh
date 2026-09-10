#!/bin/sh
# Runs every case in test/cases against the tool and diffs the output.
#
#   test/run.sh <path-to-c-contracts> [filter]
#
# A case is a .c file. Its expected output is the .expected beside it, with the
# case's directory replaced by CASE so the diff does not depend on where the
# tree lives. Set UPDATE=1 to rewrite the .expected files from what the tool
# actually printed -- read the diff before you do.
set -u

TOOL=${1:?usage: run.sh <path-to-c-contracts> [filter]}
FILTER=${2:-}
DIR=$(cd "$(dirname "$0")" && pwd)
INCLUDE=$DIR/../include

# Portable C89 is what the header promises, so it is what the cases are checked
# under. -Wall so a case that provokes an ordinary warning shows it.
CFLAGS="-std=c89 -Wall -I$INCLUDE"

pass=0
fail=0
failed=

for case in "$DIR"/cases/*.c; do
  name=$(basename "$case" .c)
  case "$name" in
    *$FILTER*) ;;
    *) continue ;;
  esac

  expected=$DIR/cases/$name.expected
  # Listing goes to stdout and diagnostics to stderr, and the two streams are
  # not flushed in a fixed order relative to each other. Capture them apart and
  # join them in a fixed one, or the suite fails at random.
  err=$("$TOOL" --list "$case" -- $CFLAGS 2>&1 >/dev/null)
  out=$("$TOOL" --list "$case" -- $CFLAGS 2>/dev/null)
  actual=$(printf '%s\n%s' "$out" "$err" | grep -v '^$' | sed "s|$DIR/cases|CASE|g")

  if [ "${UPDATE:-0}" = 1 ]; then
    printf '%s\n' "$actual" > "$expected"
    echo "updated $name"
    continue
  fi

  if [ ! -f "$expected" ]; then
    echo "FAIL $name (no $name.expected; run with UPDATE=1)"
    fail=$((fail + 1))
    failed="$failed $name"
  elif [ "$actual" = "$(cat "$expected")" ]; then
    pass=$((pass + 1))
  else
    echo "FAIL $name"
    printf '%s\n' "$actual" | diff -u "$expected" - | sed 's/^/    /'
    fail=$((fail + 1))
    failed="$failed $name"
  fi
done

[ "${UPDATE:-0}" = 1 ] && exit 0

echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ] || { echo "failed:$failed"; exit 1; }
exit 0
