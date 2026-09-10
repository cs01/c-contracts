/* prove: zero */
/* The whole of level 3 on the simplest function there is: the frame is
   checked, the loop is discharged by its own contract rather than by an unwind
   bound, and the preconditions are shown to be satisfiable first. */
#define C_CONTRACTS_NO_PREFIX
#include <c_contracts.h>

typedef unsigned long size_t;

void zero(unsigned char *p, size_t n)
  pre     (n > 0 && n < 64)
  pre     (fresh(p, n))
  assigns (range(p, 0, n))
{
  size_t i = 0;
  while (i < n)
    assigns        (locations(i, range(p, 0, n)))
    loop_invariant (i <= n)
    decreases      (n - i)
  { p[i] = 0; i++; }
}
