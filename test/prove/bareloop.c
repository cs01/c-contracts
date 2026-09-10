/* prove: sum */
/* A loop with no contract. goto-instrument does not decline this politely --
   it aborts and dumps core -- so prove.sh has to recognise the wreckage and
   say the one thing that helps. Without this case the user sees "Aborted
   (core dumped)" and nothing else. */
#include <c_contracts.h>

typedef unsigned long size_t;

int sum(const int *p, size_t n)
  contract_pre     (n > 0 && n < 8)
  contract_pre     (contract_fresh(p, n * sizeof(int)))
  contract_writes_nothing()
{
  size_t i;
  int t = 0;
  for (i = 0; i < n; i++)
    t += p[i];
  return t;
}
