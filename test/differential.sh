#!/bin/sh
# The differential gate: what this tool lowers a contract to, against what the
# fork's own -fcontract-emit-cprover-unit lowers the same source to.
#
#   test/differential.sh <path-to-c-contracts>
#
# The fork at ~/git/llvm-contracts implements this grammar as real grammar, in
# a patched clang. It is not the product; it is the oracle. This tool reaches
# CBMC through macro expansion instead, and the two must agree about what the
# contract says -- if they drift, one of them is wrong and nothing else in
# either repo would notice.
#
# Both sides go through `c-contracts clauses`, which canonicalises away
# whitespace and parentheses, so a difference reported here is a difference in
# the intrinsics, the operators or the operands: something that changes meaning
# or cost, not something that changes formatting.
#
# The differences that DO exist are recorded in differential.expected rather
# than normalised away, because the record is the gate: a new drift, or a
# recorded one disappearing, both fail. Set UPDATE=1 to re-record, and read the
# diff before you do.
set -u

TOOL=${1:?usage: differential.sh <path-to-c-contracts>}
DIR=$(cd "$(dirname "$0")" && pwd)
INCLUDE=$DIR/../include
FORK_CLANG=${FORK_CLANG:-$HOME/git/llvm-contracts/build-arm/bin/clang}
EXPECTED=$DIR/differential.expected

# A missing oracle is a skip. It is not a pass -- nothing was compared -- and
# it is not a failure, because the fork is a second checkout that a contributor
# may reasonably not have.
if [ ! -x "$FORK_CLANG" ]; then
  echo "SKIP: no contract-aware clang at $FORK_CLANG; nothing was compared"
  echo "      (build it: ninja -C ~/git/llvm-contracts/build-arm clang)"
  exit 0
fi

W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT
actual=$W/actual

# The proof fixtures, plus cases that exist only to be lowered: a gate that
# only sees what the proofs happen to use has a hole in it.
for case in "$DIR"/prove/*.c "$DIR"/differential/*.c; do
  name=$(basename "$case" .c)

  # This tool's route: the preprocessor is the whole lowering.
  cc -E -DC_CONTRACTS_CPROVER -I "$INCLUDE" "$case" -o "$W/ours.i" 2>/dev/null
  "$TOOL" clauses "$W/ours.i" > "$W/ours.txt"

  # The fork's route: its own grammar, lowered by the compiler.
  cc -E -P -DC_CONTRACTS=1 -I "$INCLUDE" "$case" -o "$W/fork.i" 2>/dev/null
  "$FORK_CLANG" -cc1 -fsyntax-only -fc-contracts -fcontract-emit-cprover-unit \
      "$W/fork.i" > "$W/fork.c" 2>/dev/null || true
  "$TOOL" clauses "$W/fork.c" > "$W/fork.txt"

  echo "=== $name" >> "$actual"
  if [ ! -s "$W/fork.txt" ]; then
    echo "the fork lowered nothing -- check -fc-contracts accepts this case" \
      >> "$actual"
  elif diff -q "$W/ours.txt" "$W/fork.txt" >/dev/null; then
    echo "identical" >> "$actual"
  else
    diff "$W/ours.txt" "$W/fork.txt" | grep '^[<>]' >> "$actual"
  fi
done

if [ "${UPDATE:-0}" = 1 ]; then
  cp "$actual" "$EXPECTED"
  echo "re-recorded $EXPECTED"
  exit 0
fi

if [ ! -f "$EXPECTED" ]; then
  echo "FAIL: no $EXPECTED; run with UPDATE=1"
  exit 1
fi
if diff -u "$EXPECTED" "$actual" > "$W/delta"; then
  echo "the two lowerings agree as recorded"
  exit 0
fi
echo "FAIL: the two lowerings drifted from the record"
sed 's/^/    /' "$W/delta"
exit 1
