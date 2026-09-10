/* Not a case of its own: the passing half of multifile.c, listed after it. */
#include <stddef.h>
#include "c_contracts.h"

void zero(unsigned char *p, size_t n)
  contract_pre     (n > 0 && n < 64)
  contract_pre     (contract_fresh(p, n))
  contract_assigns (contract_range(p, 0, n))
{
  size_t i = 0;
  while (i < n)
    contract_assigns   (contract_locations(i, contract_range(p, 0, n)))
    contract_invariant (i <= n)
    contract_decreases (n - i)
  { p[i] = 0; i++; }
}
