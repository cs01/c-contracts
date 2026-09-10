/* The entry point for ZSTD_wildcopy's proof, written by hand.
 *
 * It is here rather than generated because the separation it assumes is a
 * property of THIS PROOF, not of the function: wildcopy's real callers hand it
 * interior pointers into one output buffer, so `contract_fresh(dst, ...)` would be a
 * false thing to write on the function itself and would oblige every caller to
 * something zstd does not do. Stating it here keeps zstd's contract saying only
 * what callers actually owe, and puts the proof's own assumption where a
 * reviewer reads it.
 *
 * The length bound is the other assumption, and it is a real limit on what this
 * proves. Removing it needs a contract on each of wildcopy's two loops, which
 * is a change to zstd's source rather than to this file. */
#include "zstd_internal.h"

void __CPROVER_assume(int);
void *__CPROVER_allocate(unsigned long, int);
unsigned long nondet_ulong(void);

void __contract_harness_ZSTD_wildcopy(void)
{
    size_t length = nondet_ulong();
    __CPROVER_assume(length > 0 && length < 0x40000000);
    BYTE *dst = __CPROVER_allocate(length + WILDCOPY_OVERLENGTH, 0);
    BYTE *src = __CPROVER_allocate(length + WILDCOPY_OVERLENGTH, 0);
    ZSTD_wildcopy(dst, src, length, ZSTD_no_overlap);
}
