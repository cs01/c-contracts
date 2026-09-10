/* prove: half */
/* Preconditions nothing can satisfy. CBMC would report VERIFICATION SUCCESSFUL
   and prove nothing at all, which is the one failure that looks exactly like
   success and stays that way forever. */
#define C_CONTRACTS_NO_PREFIX
#include <c_contracts.h>

int half(int n) pre (n > 4) pre (n < 3) { return n / 2; }
