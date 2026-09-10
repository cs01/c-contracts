/* Where does x sit in the range [lo, hi], as a percentage?
 *
 * The divisor is hi - lo, which is zero when the range is empty. Clang cannot
 * fold that, so it says nothing about any of these. CBMC tells them apart.
 *
 *   clang -fsyntax-only -I../include demo.c             (silent)
 *   ./prove.sh scale examples/demo.c -Iinclude          (VERIFICATION FAILED)
 *   ./prove.sh scale_checked examples/demo.c -Iinclude  (VERIFICATION SUCCESSFUL)
 *   ./prove.sh scale_ranged examples/demo.c -Iinclude   (VERIFICATION SUCCESSFUL)
 */
#include "c_contracts.h"

unsigned divide(unsigned a, unsigned b)
  contract_pre (b != 0)
{
  return a / b;
}

/* Nothing rules out an empty range. */
unsigned scale(unsigned x, unsigned lo, unsigned hi) {
  return divide((x - lo) * 100, hi - lo);  /* clang silent, CBMC FAILS: hi == lo */
}

/* Rules it out at runtime. */
unsigned scale_checked(unsigned x, unsigned lo, unsigned hi) {
  if (hi <= lo) return 0;
  return divide((x - lo) * 100, hi - lo);  /* clang silent, CBMC PASSES: branch above */
}

/* Rules it out in the contract. */
unsigned scale_ranged(unsigned x, unsigned lo, unsigned hi)
  contract_pre (hi > lo)
{
  return divide((x - lo) * 100, hi - lo);  /* clang silent, CBMC PASSES: precondition */
}
