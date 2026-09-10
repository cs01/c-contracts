/* A branch the compiler folds away leaves an edge in the CFG with no block
   behind it, and the call-site pass has to walk past it.

   This is a positive test on purpose: the pass must still reach the call below
   and report it. Asserting only that the tool does not crash would pass just as
   well with the pass disabled -- and the crash this pins was found on zstd,
   whose decoder is full of `if (constant)`. */
#include <c_contracts.h>

void sink(int n) contract_pre (n > 0);

void folded(void)
{
  int n = 0;
  if (0)
    sink(1);
  while (0)
    sink(1);
  if (1)
    sink(1);
  else
    sink(n);
  sink(n);
}
