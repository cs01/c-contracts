#!/bin/sh
# The README's claims, checked.
#
#   test/readme.sh <path-to-c-contracts>
#
# Every code block in a README is a claim, and this one shipped three broken
# ones in two days: an example that did not compile, an example that did not
# produce the output printed under it, and a table naming syntax the header
# drops and goto-cc then rejects. None of that is caught by reading.
#
# So: every ```c block that is a whole translation unit gets compiled, and every
# contract_ name the prose mentions has to exist in the header.
set -u

TOOL=${1:?usage: readme.sh <path-to-c-contracts>}
DIR=$(cd "$(dirname "$0")" && pwd)
README=$DIR/../README.md
HEADER=$DIR/../include/c_contracts.h
INCLUDE=$DIR/../include
CC=${CC:-cc}
W=$(mktemp -d); trap 'rm -rf "$W"' EXIT

PASS=0; FAIL=0
ok()  { PASS=$((PASS + 1)); printf '  %-52s ok\n' "$1"; }
bad() { FAIL=$((FAIL + 1)); printf '  %-52s FAIL  %s\n' "$1" "$2"; }

echo "== README =="

# Split out the fenced c blocks. A block with an #include is a whole
# translation unit and must compile; the rest are fragments quoted in prose.
awk '/^```c$/{n++; f=sprintf("'"$W"'/block%02d.c", n); next}
     /^```$/{f=""; next}
     f{print > f}' "$README"

n=0
for b in "$W"/block*.c; do
  [ -f "$b" ] || continue
  grep -q '#include' "$b" || continue
  n=$((n + 1))
  name=$(basename "$b" .c)
  if $CC -fsyntax-only -std=c89 -pedantic -Wall -Wextra -I "$INCLUDE" "$b" \
       > "$b.log" 2>&1; then
    ok "$name compiles"
  else
    bad "$name compiles" "$(head -1 "$b.log")"
  fi
done
[ "$n" -gt 0 ] || bad "found a complete example to compile" "none in the README"

# Names the prose promises the reader can type.
MISSING=
for sym in $(grep -oE 'contract_[a-z_]+' "$README" | sort -u); do
  grep -qE "define $sym\\b|define $sym\\(" "$HEADER" || MISSING="$MISSING $sym"
done
if [ -z "$MISSING" ]; then ok "every contract_ name it mentions exists"
else bad "every contract_ name it mentions exists" "missing:$MISSING"; fi

# Flags it documents have to be real, or the first thing a reader tries fails.
# Only the flag tables: prose also names CBMC's own flags, which are not ours.
UNREAL=
for flag in $(grep -oE '^\| `--[a-z0-9-]+' "$README" | tr -d '|` ' | sort -u); do
  "$TOOL" --help 2>&1 | grep -q -- "$flag" || UNREAL="$UNREAL $flag"
done
if [ -z "$UNREAL" ]; then ok "every flag it documents is real"
else bad "every flag it documents is real" "not flags:$UNREAL"; fi

# Gates it lists have to exist.
GONE=
for g in $(grep -oE 'test/[a-z]+\.sh' "$README" | sort -u); do
  [ -x "$DIR/../$g" ] || GONE="$GONE $g"
done
if [ -z "$GONE" ]; then ok "every gate it lists exists"
else bad "every gate it lists exists" "missing:$GONE"; fi

echo
echo "$PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ] || exit 1
exit 0
