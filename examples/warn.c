/* Which call sites does clang warn about?
 *
 *   clang -fsyntax-only -I../include warn.c
 */
#include "c_contracts.h"

unsigned divide(unsigned a, unsigned b)
  contract_pre (b != 0)
{
  return a / b;
}

enum { ZERO = 0 };
unsigned opaque(void);

void test(void) {
  divide(1, 0);                                          /* clang warns, CBMC proves */
  divide(1, ZERO);                                       /* clang warns, CBMC proves */
  const unsigned b1 = 0; divide(1, b1);                  /* clang warns, CBMC proves */
  unsigned b2 = 0; divide(1, b2);                        /* clang silent, CBMC proves */
  unsigned b3 = 0; if (opaque()) b3 = 5; divide(1, b3); /* clang silent, CBMC proves */
  divide(1, opaque());                                   /* clang silent, CBMC proves */
}
