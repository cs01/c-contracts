/* prove: wrong */
/* A tool that only ever agrees is worth nothing. */
#include <c_contracts.h>

int wrong(int n) contract_pre (n > 0) contract_pre (n < 1000) contract_returns (contract_result > contract_old(n)) { return n; }
