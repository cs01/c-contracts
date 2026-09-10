/* prove: bump */
/* What a by-value parameter means inside a postcondition. CBMC reads it as the
   value at ENTRY -- the same thing old(n) names -- so this clause is false for
   a body that reassigns n, and must fail.

   The fork refuses this spelling outright and demands old(n); differential.sh
   records that. The refusal is strictness rather than soundness: measured here,
   the two spellings verify identically under CBMC. */
#define C_CONTRACTS_NO_PREFIX
#include <c_contracts.h>

int bump(int n) pre (n > 0 && n < 100) returns (c_result == n)
{
  n = n + 1;
  return n;
}
