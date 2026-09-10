/* A prototype that names no parameters gives a clause nothing to be checked
   against, and that has to be said rather than passed over. */
#include <c_contracts.h>

typedef unsigned long size_t;

size_t shrink(void *, size_t) contract_returns (contract_result > 0);
