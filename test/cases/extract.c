/* Every clause the header can leave on a declaration is found, in the right
   place, with the predicate the author wrote. */
#include <c_contracts.h>

typedef unsigned long size_t;

int *allocate(size_t n) contract_pre(n > 0);

size_t decode(void *dst, size_t dstCap, const void *src, size_t srcSize)
  contract_writes  (dst, dstCap)
  contract_reads   (src, srcSize)
  contract_returns (contract_result <= contract_old(dstCap));

void clear(int *p, size_t n)
  contract_writes_n (p, n)
  contract_post     (p[0] == 0);

/* A contract split across a declaration and its definition is one contract. */
int width(int n) contract_pre(n >= 0);
int width(int n) { return n; }
