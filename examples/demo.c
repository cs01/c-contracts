/* Three callers of divide. Clang is silent on all three -- the arguments are
 * variables, not constants it can fold. CBMC tells them apart.
 *
 *   clang -fsyntax-only -I../include demo.c             (silent)
 *   ./prove.sh ratio_unsafe examples/demo.c -Iinclude   (VERIFICATION FAILED)
 *   ./prove.sh ratio_checked examples/demo.c -Iinclude  (VERIFICATION SUCCESSFUL)
 *   ./prove.sh ratio_safe examples/demo.c -Iinclude     (VERIFICATION SUCCESSFUL)
 */
#include "c_contracts.h"

unsigned divide(unsigned a, unsigned b)
  contract_pre (b != 0)
{
  return a / b;
}

/* Nothing rules out b == 0. */
unsigned ratio_unsafe(unsigned a, unsigned b) {
  return divide(a, b);
}

/* Discharges the obligation with a runtime check. */
unsigned ratio_checked(unsigned a, unsigned b) {
  if (b == 0) return 0;
  return divide(a, b);
}

/* Passes the obligation up to its own caller. */
unsigned ratio_safe(unsigned a, unsigned b)
  contract_pre (b != 0)
{
  return divide(a, b);
}
