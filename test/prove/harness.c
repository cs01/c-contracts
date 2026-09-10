/* prove: check -H */
/* Harness mode. `fill` states its buffer with contract_writes and no
   contract_fresh, so there is no object for a generated entry point to
   allocate and enforce mode has no frame to check. The entry point here
   supplies one.

   What is still checked is everything except fill's own frame: its
   preconditions are asserted at the call, its loop contract discharges the
   loop, and the memory-safety checks run over the whole thing. Point -H at a
   harness that allocates one byte too few and this fails, which is what makes
   it a test of the mode rather than of CBMC. */
#include <c_contracts.h>

typedef unsigned long size_t;

void *__CPROVER_allocate(unsigned long, int);
void __CPROVER_assume(int);

void fill(unsigned char *p, size_t n) contract_writes (p, n)
{
  size_t i = 0;
  while (i < n)
    contract_assigns   (contract_locations(i, contract_range(p, 0, n)))
    contract_invariant (i <= n)
    contract_decreases (n - i)
  { p[i] = 0; i++; }
}

void check(void)
{
  size_t n;
  unsigned char *p;
  __CPROVER_assume(n > 0 && n < 32);
  p = __CPROVER_allocate(n, 0);
  fill(p, n);
}
