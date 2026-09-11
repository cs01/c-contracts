#!/bin/sh
# The header on its own, with nothing installed but a C compiler.
#
#   test/header.sh [cc]
#
# c_contracts.h is meant to be copied into a project and committed there. That
# makes it the one artifact whose tests have to travel with it: a project that
# vendors the header and edits it -- everybody edits it -- has no clang fork to
# run lit against and no LLVM to build the tool with. So nothing below needs
# either. `cc` is the whole toolchain.
#
# What is checked is the header's promise, one claim per case: it constrains
# nothing it is included into, it says which version it is, and each target
# expands every clause to what that target is supposed to see.
set -u

CC=${1:-cc}
DIR=$(cd "$(dirname "$0")" && pwd)
INCLUDE=$DIR/../include
HEADER=$INCLUDE/c_contracts.h
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT

PASS=0
FAIL=0
ok()   { PASS=$((PASS + 1)); printf '  %-52s ok\n' "$1"; }
bad()  { FAIL=$((FAIL + 1)); printf '  %-52s FAIL  %s\n' "$1" "$2"; }
check(){ if [ "$2" = 1 ]; then ok "$1"; else bad "$1" "$3"; fi }

# The sample carries one of everything, so a target that drops a clause it
# should keep, or keeps one it should drop, shows up here rather than in a
# project six months later.
cat > "$W/sample.c" <<'EOF'
#include <c_contracts.h>
typedef unsigned long size_t;
size_t decode(unsigned char *dst, size_t cap, const unsigned char *src, size_t n)
  contract_reads   (src, n)
  contract_writes  (dst, cap)
  contract_pre     (contract_fresh(dst, cap))
  contract_pre     (contract_disjoint(dst, src))
  contract_post    (contract_old(cap) > 0)
  contract_returns (contract_result <= contract_old(cap))
  contract_assigns (contract_range(dst, 0, cap));
void nothing(int n) contract_pre ((contract_ssize_t)n > 0) contract_writes_nothing();
void quantified(const unsigned char *p, size_t n)
  contract_pre (contract_readable(p, n))
  contract_pre (contract_pointer_offset(p) >= 0)
  contract_pre (contract_forall(i, 0, n, p[i] != 0));
void loops(unsigned char *p, size_t n) contract_pre (contract_fresh(p, n)) {
  size_t i = 0;
  while (i < n)
    contract_assigns        (i; contract_range(p, 0, n))
    contract_invariant (i <= n)
    contract_decreases      (n - i)
  { p[i] = 0; i++; }
}
EOF

echo "== c_contracts.h with nothing but $CC =="

# ---------------------------------------------------------------- self-contained
# The header must not drag anything in, or it decides the include order of every
# file that uses it. Comments may show an #include; code may not.
N=$(sed 's,/\*.*\*/,,' "$HEADER" | grep -c '^[[:space:]]*#[[:space:]]*include' || true)
check "includes nothing" "$([ "${N:-1}" -eq 0 ] && echo 1 || echo 0)" \
      "$N #include line(s) outside comments"

# ---------------------------------------------------------------- no variadics
# C89 has no __VA_ARGS__. This is the promise that separates it from the
# CONTRACT_REQUIRES(...) layers that need C99. Several assigns locations are
# separated with semicolons instead of a variadic macro.
N=$(grep -c '__VA_ARGS__' "$HEADER" || true)
check "uses no variadic macros" "$([ "${N:-1}" -eq 0 ] && echo 1 || echo 0)" \
      "$N use(s) of __VA_ARGS__"

# ---------------------------------------------------------------- version
# On its own line the version would be lost among the header's own
# declarations, so it is tagged and the tag is what gets read back.
printf '#include <c_contracts.h>\nc_contracts_version_is C_CONTRACTS_VERSION\n' > "$W/v.c"
V=$($CC -E -P -I "$INCLUDE" "$W/v.c" 2>/dev/null |
    sed -n 's/^c_contracts_version_is[[:space:]]*//p' | tr -d ' \t')
check "says which version it is" \
      "$(echo "$V" | grep -qE '^[0-9]+$' && echo 1 || echo 0)" \
      "C_CONTRACTS_VERSION expanded to '$V'"

# ---------------------------------------------------------------- standalone
printf '#include <c_contracts.h>\n' > "$W/solo.c"
if $CC -fsyntax-only -std=c89 -pedantic -Wall -Wextra -I "$INCLUDE" "$W/solo.c" \
     > "$W/solo.log" 2>&1; then ok "compiles alone, c89 -pedantic -Wall -Wextra"
else bad "compiles alone, c89 -pedantic -Wall -Wextra" "$(head -1 "$W/solo.log")"; fi

# ---------------------------------------------------------------- strip target
# The headline promise: under a compiler that understands none of this, every
# clause preprocesses away to the bare declaration. Nothing left to parse, and
# nothing left needing a definition at link time.
for f in sample; do
  $CC -E -P -DC_CONTRACTS_STOCK=0 -I "$INCLUDE" "$W/$f.c" >> "$W/strip.i" 2>/dev/null
done
LEFT=$(grep -oE '__attribute__|annotate|__c_[a-z_]+|__CPROVER_[A-Za-z_]+|loop_invariant|decreases' \
         "$W/strip.i" | sort -u | tr '\n' ' ')
check "strip target leaves the bare declaration" \
      "$([ -z "$LEFT" ] && echo 1 || echo 0)" "left behind: $LEFT"

if $CC -fsyntax-only -std=c89 -pedantic -Wall -Wextra -DC_CONTRACTS_STOCK=0 \
     -I "$INCLUDE" "$W/sample.c" > "$W/strip.log" 2>&1 &&
   true; then
  ok "strip target is clean under -pedantic -Wall -Wextra"
else
  bad "strip target is clean under -pedantic -Wall -Wextra" "$(head -1 "$W/strip.log")"
fi

# ---------------------------------------------------------------- cprover target
for f in sample; do
  $CC -E -P -DC_CONTRACTS_CPROVER -I "$INCLUDE" "$W/$f.c" >> "$W/cp.i" 2>/dev/null
done
for want in __CPROVER_requires __CPROVER_ensures __CPROVER_assigns \
            __CPROVER_loop_invariant __CPROVER_decreases __CPROVER_is_fresh \
            __CPROVER_r_ok __CPROVER_w_ok __CPROVER_same_object \
            __CPROVER_object_upto __CPROVER_old __CPROVER_return_value \
            __CPROVER_POINTER_OFFSET __CPROVER_forall __CPROVER_ssize_t; do
  grep -q "$want" "$W/cp.i" || MISSING="${MISSING:-} $want"
done
check "cprover target lowers every clause" \
      "$([ -z "${MISSING:-}" ] && echo 1 || echo 0)" "missing:${MISSING:-}"
LEFT=$(grep -oE '__c_[a-z_]+|__attribute__|annotate' "$W/cp.i" | sort -u | tr '\n' ' ')
check "cprover target leaves no stock-target names" \
      "$([ -z "$LEFT" ] && echo 1 || echo 0)" "left behind: $LEFT"

# ---------------------------------------------------------------- stock target
# Only a compiler with diagnose_if reaches this branch, so it is skipped rather
# than failed elsewhere: a skip is not a pass.
printf '#ifdef __has_attribute\n#if __has_attribute(diagnose_if)\nyes\n#endif\n#endif\n' > "$W/di.c"
if $CC -E -P "$W/di.c" 2>/dev/null | grep -q yes; then
  for f in sample; do
    $CC -E -P -I "$INCLUDE" "$W/$f.c" >> "$W/stock.i" 2>/dev/null
  done
  for want in diagnose_if 'annotate("contract_returns:' 'annotate("contract_post:' \
              __contract_readable __contract_writable __contract_fresh __contract_same_object; do
    grep -q "$want" "$W/stock.i" || MISS2="${MISS2:-} $want"
  done
  check "stock target keeps what the tool reads" \
        "$([ -z "${MISS2:-}" ] && echo 1 || echo 0)" "missing:${MISS2:-}"

  # A precondition a released compiler can fold has to actually fire, or tier 1
  # is a claim rather than a feature.
  cat > "$W/fire.c" <<'EOF'
#include <c_contracts.h>
void sink(int n) contract_pre (n > 0);
void call(void) { sink(0); }
EOF
  $CC -fsyntax-only -std=c89 -Wall -I "$INCLUDE" "$W/fire.c" > "$W/fire.log" 2>&1
  check "a violated precondition warns with no tool" \
        "$(grep -q 'precondition n > 0 is violated' "$W/fire.log" && echo 1 || echo 0)" \
        "$(head -1 "$W/fire.log")"

  # And a satisfied one must not, or the check is noise.
  cat > "$W/quiet.c" <<'EOF'
#include <c_contracts.h>
void sink(int n) contract_pre (n > 0);
void call(void) { sink(1); }
EOF
  $CC -fsyntax-only -std=c89 -pedantic -Wall -Wextra -I "$INCLUDE" "$W/quiet.c" \
      > "$W/quiet.log" 2>&1
  check "a satisfied precondition is silent" \
        "$([ ! -s "$W/quiet.log" ] && echo 1 || echo 0)" "$(head -1 "$W/quiet.log")"
else
  printf '  %-52s SKIP  %s\n' "stock target" "$CC has no diagnose_if"
fi

# ---------------------------------------------------------------- double include
# A vendored header gets included by several of a project's own headers, so
# including it twice has to be free.
cat > "$W/twice.c" <<'EOF'
#include <c_contracts.h>
#include <c_contracts.h>
void g(int *p, unsigned long n) contract_reads_n (p, n) contract_pre (n > 0);
EOF
if $CC -fsyntax-only -std=c89 -pedantic -I "$INCLUDE" "$W/twice.c" > "$W/twice.log" 2>&1
then ok "including it twice is harmless"
else bad "including it twice is harmless" "$(head -1 "$W/twice.log")"; fi

echo
echo "$PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ] || exit 1
exit 0
