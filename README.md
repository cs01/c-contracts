# c-contracts

Contracts for C in one header. Write preconditions, postconditions, and frame
conditions on your functions; clang type-checks them on every build, and
[CBMC](https://www.cprover.org/cbmc/) proves them for all possible inputs.

```c
#include "c_contracts.h"

unsigned divide(unsigned a, unsigned b)
  contract_pre (b != 0)
{
  return a / b;
}
```

```
$ clang -fsyntax-only -Iinclude examples/warn.c
examples/warn.c:17:14: warning: precondition b != 0 is violated by this call
   17 |   divide(1, 0);
      |              ^
$ ./prove.sh divide examples/demo.c -Iinclude
VERIFICATION SUCCESSFUL
```

## Installation

```sh
curl -O https://raw.githubusercontent.com/cs01/c-contracts/main/include/c_contracts.h
```

C89, no includes of its own. Proving needs [CBMC](https://www.cprover.org/cbmc/)
6+ (`goto-cc`, `goto-instrument`, `cbmc`).

## Three targets

The same annotations mean three different things depending on what compiles them:

| compiler | mechanism | check |
|---|---|---|
| CBMC (`-DC_CONTRACTS_CPROVER`) | `__CPROVER_requires` etc. | exhaustive proof over all inputs |
| stock clang | `diagnose_if` attribute | warnings at call sites it can fold |
| GCC / MSVC / tcc | expands to nothing | zero checking, zero overhead |

[`examples/warn.c`](examples/warn.c) — which call sites clang can see:

```c
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
```

```
$ clang -fsyntax-only -Iinclude examples/warn.c
examples/warn.c:17:14: warning: precondition b != 0 is violated by this call
examples/warn.c:18:17: warning: precondition b != 0 is violated by this call
examples/warn.c:19:38: warning: precondition b != 0 is violated by this call
```

## Try it

[`examples/demo.c`](examples/demo.c) — the divisor is `hi - lo`, zero on an
empty range. Clang cannot fold it, so it is silent on all three:

```c
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
```

```
$ ./prove.sh scale examples/demo.c -Iinclude
[divide.division-by-zero.1] line 16 division by zero in a / b: FAILURE
VERIFICATION FAILED

$ ./prove.sh scale_checked examples/demo.c -Iinclude
VERIFICATION SUCCESSFUL

$ ./prove.sh scale_ranged examples/demo.c -Iinclude
VERIFICATION SUCCESSFUL
```

For the last one CBMC has to prove that `hi > lo` implies `hi - lo != 0` — a
fact about the caller's argument becoming a fact about the callee's.

## Generating proofs

```
./prove.sh <function> <source.c> [-I dir ...] [-r dep ...] [-H] [-- cbmc flags]
```

1. **Preprocess** with `-DC_CONTRACTS_CPROVER`, so clauses become `__CPROVER_*`.
2. **Compile** to a goto program with `goto-cc`.
3. **Instrument** — `goto-instrument` applies loop contracts, enforces the named
   function's contract, and derives the entry point from its preconditions.
4. **Prove** — `cbmc` checks every reachable property. Solvers race; first clean
   answer wins.

A vacuity check follows a successful proof: unsatisfiable preconditions make
everything hold for free, so `prove.sh` exits with an error instead.

## Modular verification

Prove a leaf, then pass `-r` so callers trust its contract instead of
re-analyzing its body. Each proof stays small no matter how deep the call tree.

```
$ ./prove.sh compress_bound source.c
$ ./prove.sh compress source.c -r compress_bound
```

If a contract uses `contract_readable`/`contract_writable` without
`contract_fresh`, there is no object for CBMC to allocate. Write your own entry
point and pass `-H` to skip frame enforcement:

```
$ ./prove.sh my_harness source.c -H
```

## Reference

### Clauses

A clause goes after the parameter list, before the `;` or `{`. They stack.
Everything below is shorthand for a clause, or vocabulary you use inside one.

| clause | means |
|---|---|
| `contract_pre (P)` | caller must establish `P` |
| `contract_post (P)` | `P` holds on return |
| `contract_returns (P)` | `P` holds on return, may name `contract_result` |
| `contract_assigns (L)` | nothing outside `L` changes |
| `contract_frees (L)` | nothing outside `L` is freed |
| `contract_writes_nothing()` | the frame is empty |

### Predicates

The vocabulary for memory, which C has no syntax for. These go *inside* a
clause: `contract_pre (contract_fresh(p, n))` specifies something,
`contract_fresh(p, n)` alone does not.

| predicate | true when |
|---|---|
| `contract_readable (p, n)` | `n` bytes at `p` may be read |
| `contract_writable (p, n)` | `n` bytes at `p` may be written |
| `contract_fresh (p, n)` | `p` is an object of exactly `n` bytes that nothing else aliases |
| `contract_same_object (p, q)` | `p` and `q` point into one object |
| `contract_disjoint (p, q)` | they do not |
| `contract_freeable (p)` | `p` is a legally freeable allocation |
| `contract_was_freed (p)` | `p` was freed during the call. `contract_post` only |
| `contract_forall (i, lo, hi, P)` | `P` holds for every `i` in `[lo, hi)` |
| `contract_exists (i, lo, hi, P)` | `P` holds for some `i` in `[lo, hi)` |
| `contract_obeys (f, c)` | function pointer `f` satisfies contract `c` |

### Values

| value | is |
|---|---|
| `contract_old (E)` | `E` evaluated at function entry |
| `contract_loop_entry (E)` | `E` evaluated before the first loop iteration |
| `contract_result` | the return value. `contract_returns` only |
| `contract_pointer_offset (p)` | `p`'s offset within its own object |
| `contract_ssize_t` | a signed type wide enough to hold that offset |

### Frame locations

Memory a function may write. These go inside `contract_assigns (...)`. A bare
lvalue is also a location, so `contract_assigns (i)` says `i` and nothing else.

| location | is |
|---|---|
| `contract_range (p, lo, hi)` | elements `[lo, hi)` of `p`, half open |
| `contract_object_whole (p)` | the entire object `p` points into |
| `contract_object_from (p)` | from `p` to the end of its object |

`contract_assigns` takes a single argument because the header promises C89, and
C89 has no `__VA_ARGS__`. Separate several locations with semicolons, which the
preprocessor does not treat as argument separators:

```c
contract_assigns (op; ip; contract_range(dst, 0, n))
```

A comma-separated list works too, but only inside one set of parentheses that
the preprocessor already sees as a single argument. Semicolons are the form
that always works, on function and loop clauses alike.

### Loop clauses

These go on a loop, between the header and the body. With them CBMC proves the
loop by induction rather than unwinding it, so the result holds for every input
rather than up to a bound.

```c
while (i < n)
  contract_assigns   (i; contract_range(p, 0, n))
  contract_invariant (i <= n)
  contract_decreases (n - i)
{ p[i] = 0; i++; }
```

`contract_ghost` marks a variable that exists only for an annotation, so it does
not trip `-Wunused-variable` on targets where clauses vanish:

```c
BYTE* const opStart contract_ghost = op;
```

### Roles

A role expands to several clauses; anything it says you can also write by hand.

**Watch the units.** `contract_reads`/`contract_writes` count **bytes**, like
`memcpy`. The `_n` forms count **elements**. `contract_range(p, lo, hi)` also
counts elements, and is half open.

| role | expands to |
|---|---|
| `contract_reads (p, n)` | `contract_pre (p != 0)`, `contract_pre (contract_readable(p, n))` |
| `contract_writes (p, n)` | `contract_pre (p != 0)`, `contract_pre (contract_writable(p, n))`, and a frame of `n` bytes at `p` |
| `contract_reads_n (p, n)` | as `contract_reads`, over `n * sizeof(*p)` bytes |
| `contract_writes_n (p, n)` | as `contract_writes`, over `n * sizeof(*p)` bytes |

A function that reads then writes carries both roles.

## Gotchas

- **Write `0`, not `NULL`.** `contract_pre (p != NULL)` does not compile on
  clang: `NULL` is `((void *)0)`, and a cast to a pointer is not a constant
  expression in C.
- **`contract_writes` says nothing about aliasing.** Two roles on one call may
  name the same buffer. Say `contract_pre (contract_disjoint(a, b))` or
  `contract_pre (contract_fresh(p, n))` if you mean it.

## Tests

Nothing to build — the product is a header and a shell script, so these gates
are the whole of CI. `test/run.sh` runs them all; each also runs alone, and
skips loudly if its prerequisite is missing.

| gate | needs |
|---|---|
| `test/header.sh` | a C compiler |
| `test/readme.sh` | a C compiler. Compiles this file's examples |
| `test/prove.sh` | CBMC. Runs `prove.sh` over `test/prove/` |
