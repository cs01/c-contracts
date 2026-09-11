/* prove: zero */
/* The frame gate, pinned from the side that can fail. enforce.c passing does
   not distinguish a checked frame from an ignored one; this does. The function
   and its loop both claim one byte and the loop writes n of them.

   Narrowing only the function's clause would NOT fail here: CBMC lets a loop's
   own assigns widen the frame inside the loop and never checks that the loop's
   targets lie within the function's. That is worth knowing before trusting a
   frame on a function whose loops are annotated. */
#include <c_contracts.h>

typedef unsigned long size_t;

void zero(unsigned char *p, size_t n)
  contract_pre     (n > 1 && n < 64)
  contract_pre     (contract_fresh(p, n))
  contract_assigns (contract_range(p, 0, 1))
{
  size_t i = 0;
  while (i < n)
    contract_assigns        (i; contract_range(p, 0, 1))
    contract_invariant (i <= n)
    contract_decreases      (n - i)
  { p[i] = 0; i++; }
}
