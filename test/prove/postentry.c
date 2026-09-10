/* prove: bump */
/* What a by-value parameter means inside a postcondition. CBMC reads it as the
   value at ENTRY -- the same thing contract_old(n) names -- so this clause is false for
   a body that reassigns n, and must fail. Write contract_old(n) when the entry
   value is what you mean, which is almost always. */
#include <c_contracts.h>

int bump(int n) contract_pre (n > 0 && n < 100) contract_returns (contract_result == n)
{
  n = n + 1;
  return n;
}
