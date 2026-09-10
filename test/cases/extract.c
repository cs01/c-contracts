/* Every clause the header can leave on a declaration is found, in the right
   place, with the predicate the author wrote. */
#define C_CONTRACTS_NO_PREFIX
#include <c_contracts.h>

typedef unsigned long size_t;

int *allocate(size_t n) pre(n > 0);

size_t decode(void *dst, size_t dstCap, const void *src, size_t srcSize)
  writes  (dst, dstCap)
  reads   (src, srcSize)
  returns (c_result <= old(dstCap));

void clear(int *p, size_t n)
  writes_n (p, n)
  post     (p[0] == 0);

/* A contract split across a declaration and its definition is one contract. */
int width(int n) pre(n >= 0);
int width(int n) { return n; }
