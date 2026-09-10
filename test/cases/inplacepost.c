/* A `post` is checked where it is written, by clang itself, before this tool
 * ever runs; a `returns` cannot be, so it still reaches the ghost scope. Both
 * kinds of error have to surface, and neither twice. */
#include <c_contracts.h>

typedef unsigned long size_t;

/* Checked in place: the parameters are in scope where diagnose_if parses this,
 * and contract_old(E) is E in a scope whose parameters are the entry values. */
size_t good(int *p, size_t n)
  contract_post    (p != 0 && n <= 64)
  contract_post    (n == contract_old(n))
  contract_returns (contract_result <= n);

/* The post fails in place, the returns fails in the ghost. */
size_t both(int *p, size_t n)
  contract_post    (nn > 0)
  contract_returns (contract_result <= mm);

/* The folded condition must never fire at a call site. */
void caller(void) { int x; good(&x, 1); }
