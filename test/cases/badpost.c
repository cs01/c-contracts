/* Clauses that stock clang only lexed, checked in a scope built for them. */
#include <c_contracts.h>

typedef unsigned long size_t;

/* Typo in a name that is in scope. */
size_t a(void *dst, size_t dstCap) contract_returns (contract_result <= dstCapp);

/* A void function has no result to name. */
void b(int *p) contract_returns (contract_result == 0);

/* The predicate is not a condition. */
struct S { int x; };
struct S c(int n) contract_returns (contract_result <= n);

/* Well formed, and stays silent. */
size_t d(size_t n) contract_returns (contract_result <= n);

/* contract_old() of a parameter is the parameter, in a scope that has not run yet. */
size_t e(size_t n) contract_returns (contract_result <= contract_old(n));
