/* prove: zero */
/* The whole of level 3 on the simplest function there is: the frame is
   checked, the loop is discharged by its own contract rather than by an unwind
   bound, and the preconditions are shown to be satisfiable first. */
#include <c_contracts.h>

typedef unsigned long size_t;

void zero(unsigned char *p, size_t n)
  contract_pre     (n > 0 && n < 64)
  contract_pre     (contract_fresh(p, n))
  contract_assigns (contract_range(p, 0, n))
{
  size_t i = 0;
  while (i < n)
    contract_assigns        (i; contract_range(p, 0, n))
    contract_invariant (i <= n)
    contract_decreases      (n - i)
  { p[i] = 0; i++; }
}
