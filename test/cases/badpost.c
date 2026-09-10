/* Clauses that stock clang only lexed, checked in a scope built for them. */
#define C_CONTRACTS_NO_PREFIX
#include <c_contracts.h>

typedef unsigned long size_t;

/* Typo in a name that is in scope. */
size_t a(void *dst, size_t dstCap) returns (c_result <= dstCapp);

/* A void function has no result to name. */
void b(int *p) returns (c_result == 0);

/* The predicate is not a condition. */
struct S { int x; };
struct S c(int n) returns (c_result <= n);

/* Well formed, and stays silent. */
size_t d(size_t n) returns (c_result <= n);

/* old() of a parameter is the parameter, in a scope that has not run yet. */
size_t e(size_t n) returns (c_result <= old(n));
