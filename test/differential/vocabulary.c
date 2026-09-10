/* Every spelling in the annotation layer, on one declaration.
 *
 * This case never runs a proof; it exists so that differential.sh compares the
 * two lowerings of the WHOLE vocabulary. A gate that only sees what the proof
 * fixtures happen to use is a gate with a hole in it: swapping w_ok for r_ok
 * inside contract_writable passed cleanly until this file existed. */
#include <c_contracts.h>

typedef unsigned long size_t;

int decode(unsigned char *dst, size_t dstCap,
           const unsigned char *src, size_t srcSize)
  contract_reads   (src, srcSize)
  contract_writes  (dst, dstCap)
  contract_pre     (contract_readable(src, srcSize))
  contract_pre     (contract_writable(dst, dstCap))
  contract_pre     (contract_fresh(dst, dstCap))
  contract_pre     (contract_same_object(src, src))
  contract_pre     (contract_disjoint(dst, src))
  contract_pre     (contract_pointer_offset(src) >= 0)
  contract_pre     (contract_forall(i, 0, srcSize, src[i] != 0))
  contract_post    (contract_old(dstCap) > 0)
  contract_returns (contract_result <= (int)contract_old(dstCap))
  contract_assigns (contract_locations(contract_range(dst, 0, dstCap), contract_range(dst, 0, 1)));

void sink(int *p, size_t n) contract_reads_n (p, n) contract_writes_n (p, n);

void reader(int n) contract_pre ((contract_ssize_t)n > 0) contract_writes_nothing;
