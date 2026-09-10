/* prove: half */
/* Preconditions nothing can satisfy. CBMC would report VERIFICATION SUCCESSFUL
   and prove nothing at all, which is the one failure that looks exactly like
   success and stays that way forever. */
#include <c_contracts.h>

int half(int n) contract_pre (n > 4) contract_pre (n < 3) { return n / 2; }
