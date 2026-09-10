/* The call-site pass: preconditions checked against what the CFG knows about
   the arguments. Clang's own diagnose_if folds only against the argument
   expressions, so everything that travels through a variable reaches nobody
   without this. Each case below says what it pins. */
#define C_CONTRACTS_NO_PREFIX
#include <c_contracts.h>

typedef unsigned long size_t;

int *allocate(size_t n) pre(n > 0);
int *bounded(size_t n) pre(n < 10);
int opaque(void);

/* 1. The point of the pass. The violation is visible only through a variable,
   which is exactly what a released clang cannot fold. */
void through_a_variable(void)
{
  size_t n = 0;
  allocate(n);
}

/* 1b. A literal argument is what clang already folds on its own. Included to
   confirm this pass catches it too, not because it is what the pass is for. */
void a_literal(void)
{
  allocate(0);
}

/* 2. Silence when genuinely unknown. A value from a source the pass cannot
   analyse must never produce a report; this is the case that decides whether
   the pass is usable at all. */
void from_elsewhere(void)
{
  size_t n = (size_t)opaque();
  allocate(n);
}

/* 3. The fixpoint. A single CFG sweep skips the back edge, so the loop header
   keeps the pre-loop state, `n` still looks like 0 at the call, and the pass
   invents a report. This case is the regression test for iterating.

   The bound is a parameter rather than a literal on purpose. With `i < 10` the
   pre-loop state pins i to 0, which makes the loop's false edge look
   infeasible, so a single sweep drops the call's block instead of misjudging
   it -- wrong in the same way, but silently, and a silent break gates
   nothing. */
void after_a_loop(int limit)
{
  size_t n = 0;
  int i;
  for (i = 0; i < limit; i++)
    n = (size_t)(i + 1);
  allocate(n);
}

/* 4. The branch merge keeps only what every predecessor agrees on: two
   different values are not one value.

   Both values violate the callee's bound on their own, so a merge that kept
   any single predecessor's facts would report here whichever one it kept.
   Pairing a known value with an unknown one would leave the outcome up to
   which predecessor the CFG happens to list first. */
void merged(int c)
{
  size_t n;
  if (c)
    n = 20;
  else
    n = 30;
  bounded(n);
}

/* 5. The function's own preconditions seed the entry state, so a call whose
   precondition this function's caller already guaranteed stays silent. */
void forwards(size_t n) pre(n > 0)
{
  allocate(n);
}

/* 5b. The same, in the ordinary header-plus-.c layout. The contract is on the
   prototype, so the entry facts are seeded from the prototype's parameters
   while the body names the definition's -- distinct ParmVarDecls for the same
   parameter. This one must report: its own precondition pins n to 0, which is
   exactly what allocate forbids. Case 5 is a negative test and so passes
   whether or not seeding works at all; this is the one that fails when it
   does not. */
void forwards_split(size_t n) pre(n == 0);
void forwards_split(size_t n)
{
  allocate(n);
}

/* 6. The contract is spelled on the prototype and the definition is separate,
   so the predicate names the prototype's parameters while the call resolves to
   the definition. Pins that substitution is keyed on the parameter index. */
int scale(int k) pre(k != 0);
int scale(int k) { return k; }

void calls_scale(void)
{
  int k = 0;
  scale(k);
}

/* 7. A range that does not imply the callee's bound is reported separately,
   because it is a maybe rather than a definite violation. */
void a_range(size_t n)
{
  if (n > 2)
    bounded(n);
}
