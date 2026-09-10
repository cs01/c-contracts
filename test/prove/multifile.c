/* prove: zero */
/* also: multifile_ok.c */
/* A failing file followed by a passing one must still exit non-zero.
   The exit status was assigned per translation unit rather than accumulated,
   so whichever file came last decided it, and a CI job proving a directory
   reported success whenever its last file happened to pass. */
#include <stddef.h>
#include "c_contracts.h"

void zero(unsigned char *p, size_t n)
  contract_pre     (n > 1 && n < 64)
  contract_pre     (contract_fresh(p, n))
  contract_assigns (contract_range(p, 0, 1))
{
  size_t i = 0;
  while (i < n)
    contract_assigns   (contract_locations(i, contract_range(p, 0, 1)))
    contract_invariant (i <= n)
    contract_decreases (n - i)
  { p[i] = 0; i++; }
}
