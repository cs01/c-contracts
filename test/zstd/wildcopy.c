/* A translation unit whose only job is to put ZSTD_wildcopy in front of
 * prove.sh, with the contract that lives in zstd's own header.
 *
 * Two things have to be arranged and neither touches the contract:
 *
 * 1. ZSTD_wildcopy is MEM_STATIC FORCE_INLINE_ATTR, so goto-cc drops it
 *    before anything can be proved about it. Blanking both gives it external
 *    linkage.
 *
 * 2. Its buffers are stated with contract_readable / contract_writable and no
 *    contract_fresh -- deliberately, because wildcopy is called on slices of
 *    buffers its callers own. That says the memory is accessible but not
 *    which object it belongs to, so there is nothing for a generated entry
 *    point to allocate and enforce mode has no frame to check. The harness
 *    below supplies the two objects, and prove.sh runs it with -H.
 *
 * The contract is still what is being checked: every precondition in
 * zstd_internal.h is live, and so is the loop contract inside the function.
 * What is not checked is wildcopy's own frame. */
#include <string.h>
#include <stddef.h>

/* Override zstd's builtin wrappers so CBMC gets plain memcpy/memmove. Must
   appear before zstd_deps.h, which is pulled in by zstd_internal.h. */
#define ZSTD_DEPS_NEED_MALLOC
#include "zstd_deps.h"
#undef ZSTD_memcpy
#undef ZSTD_memmove
#undef ZSTD_memset
#define ZSTD_memcpy(d,s,l) memcpy((d),(s),(l))
#define ZSTD_memmove(d,s,l) memmove((d),(s),(l))
#define ZSTD_memset(d,v,l) memset((d),(v),(l))

#define MEM_STATIC
#define FORCE_INLINE_ATTR
#include "zstd_internal.h"

void *__CPROVER_allocate(unsigned long, int);
void __CPROVER_assume(int);

/* Symbolically sized, not a fixed length: the bound is only what keeps the
   solver finite, and the loop inside wildcopy is discharged by its own
   contract rather than by an unwind bound. WILDCOPY_OVERLENGTH is the slack
   the contract requires every caller to own. */
void harness(void)
{
  size_t length;
  __CPROVER_assume(length < 4096);
  BYTE *dst = __CPROVER_allocate(length + WILDCOPY_OVERLENGTH, 0);
  const BYTE *src = __CPROVER_allocate(length + WILDCOPY_OVERLENGTH, 0);
  ZSTD_wildcopy(dst, src, length, ZSTD_no_overlap);
}
