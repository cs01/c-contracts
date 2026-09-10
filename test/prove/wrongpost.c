/* prove: wrong */
/* A tool that only ever agrees is worth nothing. */
#define C_CONTRACTS_NO_PREFIX
#include <c_contracts.h>

int wrong(int n) pre (n > 0) pre (n < 1000) returns (c_result > old(n)) { return n; }
