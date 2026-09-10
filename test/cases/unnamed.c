/* A prototype that names no parameters gives a clause nothing to be checked
   against, and that has to be said rather than passed over. */
#define C_CONTRACTS_NO_PREFIX
#include <c_contracts.h>

typedef unsigned long size_t;

size_t shrink(void *, size_t) returns (c_result > 0);
