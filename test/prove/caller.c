/* prove: copy */
/* disjoint() only bites on the caller's side: enforcing copy's contract
   ASSUMES the two buffers do not overlap, and only a caller checked against
   that contract has to prove it. caller_alias passes the same buffer twice and
   must fail; caller_ok must not. Delete the disjoint clause and the two agree,
   which is what makes this a test of the clause rather than of CBMC. */
#define C_CONTRACTS_NO_PREFIX
#include <c_contracts.h>

typedef unsigned long size_t;

void copy(unsigned char *dst, const unsigned char *src, size_t n)
  pre     (n > 0 && n < 8)
  pre     (fresh(dst, n))
  pre     (disjoint(dst, src))
  assigns (range(dst, 0, n))
{
  size_t i;
  for (i = 0; i < n; i++)
    dst[i] = src[i];
}

void caller_alias(void) { unsigned char a[4]; copy(a, a, 4); }
void caller_ok(void) { unsigned char a[4]; unsigned char b[4]; copy(a, b, 4); }
