/* Every spelling in the annotation layer, on one declaration.
 *
 * This case never runs a proof; it exists so that differential.sh compares the
 * two lowerings of the WHOLE vocabulary. A gate that only sees what the proof
 * fixtures happen to use is a gate with a hole in it: swapping w_ok for r_ok
 * inside c_writable passed cleanly until this file existed. */
#define C_CONTRACTS_NO_PREFIX
#include <c_contracts.h>

typedef unsigned long size_t;

int decode(unsigned char *dst, size_t dstCap,
           const unsigned char *src, size_t srcSize)
  reads   (src, srcSize)
  writes  (dst, dstCap)
  pre     (readable(src, srcSize))
  pre     (writable(dst, dstCap))
  pre     (fresh(dst, dstCap))
  pre     (same_object(src, src))
  pre     (disjoint(dst, src))
  pre     (pointer_offset(src) >= 0)
  pre     (c_forall(i, 0, srcSize, src[i] != 0))
  post    (old(dstCap) > 0)
  returns (c_result <= (int)old(dstCap))
  assigns (locations(range(dst, 0, dstCap), range(dst, 0, 1)));

void sink(int *p, size_t n) reads_n (p, n) writes_n (p, n);

void reader(int n) pre ((c_ssize_t)n > 0) c_writes_nothing;
