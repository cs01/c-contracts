/* prove: zero_writes */
/* writes(p, n) says the memory is valid to write and deliberately says nothing
   about which object it belongs to, so a verifier has no object to reason
   about. The diagnostic naming the missing clause is the point of this case;
   the failures below it are what a user sees without it. */
#define C_CONTRACTS_NO_PREFIX
#include <c_contracts.h>

typedef unsigned long size_t;

void zero_writes(unsigned char *p, size_t n) writes (p, n)
{
  size_t i;
  for (i = 0; i < n; i++)
    p[i] = 0;
}
