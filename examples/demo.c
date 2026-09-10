/* Minimal runnable example.
 *
 *   clang -fsyntax-only -I../include demo.c
 *   ./prove.sh divide examples/demo.c -Iinclude
 */
#include "c_contracts.h"

unsigned divide(unsigned a, unsigned b)
  contract_pre (b != 0)
{
  return a / b;
}

void caller(void) {
  divide(10, 0);
}
