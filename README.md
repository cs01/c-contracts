# c-contracts

Contracts for C in one header. Write preconditions, postconditions, and frame
conditions on your functions. The header offers progressive checks:
* Clang type-checks them on every compile, no-op for non-clang
* [CBMC](https://www.cprover.org/cbmc/) formally verifies the contracts for all possible inputs.

```c
#include "c_contracts.h"

unsigned divide(unsigned a, unsigned b)
  contract_pre (b != 0)
{
  return a / b;
}
```

Clang warns at any call site where `b` might be zero:

```
warning: precondition b != 0 is violated by this call
    divide(10, 0);
    ^
```

Other compilers silently do nothing.

CBMC proves it for every possible value of `a` and `b`:

```
$ ./prove.sh divide examples/demo.c -Iinclude
VERIFICATION SUCCESSFUL
```

See [Try it](#try-it) for a complete runnable example.

## Installation

Download and include in your C project:

```sh
curl -O https://raw.githubusercontent.com/cs01/c-contracts/main/include/c_contracts.h
```

The header is C89 with no includes of its own.

### Dependencies
Needs [CBMC](https://www.cprover.org/cbmc/) 6+ (`goto-cc`, `goto-instrument`, `cbmc`).

## Compiler Warnings vs. Proofs

The same annotations target three compilers:

| compiler | mechanism | check |
|---|---|---|
| CBMC (`-DC_CONTRACTS_CPROVER`) | `__CPROVER_requires` / `__CPROVER_r_ok` / etc. | exhaustive formal proof over all inputs |
| stock clang | `diagnose_if` attribute | compile-time warnings at call sites where the compiler can fold constants |
| GCC / MSVC / tcc | everything expands to nothing | zero checking, zero overhead |

Save this as `warn.c` and compile with clang to see which call sites warn:

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
$ clang -fsyntax-only warn.c
warn.c:13:14: warning: precondition b != 0 is violated by this call
warn.c:14:17: warning: precondition b != 0 is violated by this call
warn.c:15:38: warning: precondition b != 0 is violated by this call
```

Clang catches constants and folded constants at compile time; anything it cannot
evaluate is silent. CBMC proves every case for every possible input.
This example is also at [`examples/warn.c`](examples/warn.c).


## Try it

Where the prover earns its keep. The divisor here is `hi - lo`, zero when the
range is empty — not something clang can fold, so it says nothing about any of
these.

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

Clang is silent on all three. CBMC tells them apart:

```
$ clang -fsyntax-only -Iinclude examples/demo.c
$ ./prove.sh scale examples/demo.c -Iinclude
[divide.division-by-zero.1] line 16 division by zero in a / b: FAILURE
VERIFICATION FAILED

$ ./prove.sh scale_checked examples/demo.c -Iinclude
VERIFICATION SUCCESSFUL

$ ./prove.sh scale_ranged examples/demo.c -Iinclude
VERIFICATION SUCCESSFUL
```

The full file is [`examples/demo.c`](examples/demo.c). Both fixes are
legitimate: check the condition at runtime, or state it as your own
precondition and hand the obligation to your caller. For the last one CBMC has
to prove that `hi > lo` implies `hi - lo != 0` — a fact about the caller's
argument becoming a fact about the callee's.

## Generating proofs

```
./prove.sh <function> <source.c> [-I dir ...] [-r dep ...] [-H] [-- cbmc flags]
```

`prove.sh` drives CBMC through four stages:

1. **Preprocess** — compiles with `-DC_CONTRACTS_CPROVER` so contract macros
   expand to `__CPROVER_requires`, `__CPROVER_ensures`, etc.
2. **Compile** — `goto-cc` produces a goto program.
3. **Instrument** — `goto-instrument` applies loop contracts and enforces the
   named function's contract, generating the entry point from its preconditions.
4. **Prove** — `cbmc` checks every reachable property. Multiple solvers race
   and the first clean answer wins.

A vacuity check runs after a successful proof: if the preconditions are
unsatisfiable, the proof is vacuous and `prove.sh` exits with an error.

## Modular verification
Once a function has been verified, you can re-use that proof for functions that
call it, rather than re-verifying the entire codebase for each function.

Once you prove a dependency, use `-r` so callers
trust its contract instead of re-analyzing its body:

```
$ ./prove.sh compress_bound source.c           # prove the leaf
$ ./prove.sh compress source.c -r compress_bound  # prove the caller
```

This scales to large codebases. Each proof stays small regardless of the
call tree below it.

If a function's contract uses `contract_readable`/`contract_writable` without
`contract_fresh`, there is no object for CBMC to allocate. Write your own entry
point and pass `-H` to skip frame enforcement:

```
$ ./prove.sh my_harness source.c -H
```

## Reference

### Clauses

A clause goes after the parameter list, before the `;` or `{`. They stack.
Everything else below is shorthand for clauses or vocabulary you use inside one.

| clause | means |
|---|---|
| `contract_pre (P)` | caller must establish `P` |
| `contract_post (P)` | `P` holds on return |
| `contract_returns (P)` | `P` holds on return, may name `contract_result` |
| `contract_assigns (L)` | nothing outside `L` changes |
| `contract_frees (L)` | nothing outside `L` is freed |
| `contract_writes_nothing()` | the frame is empty |

### Predicates

Predicates go inside clauses. They are the vocabulary for talking about memory,
which C has no syntax for. `contract_pre (contract_fresh(p, n))` is a clause
containing a predicate; `contract_fresh(p, n)` on its own specifies nothing.

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

Things you can name inside a clause that are not true or false.

| value | is |
|---|---|
| `contract_old (E)` | `E` evaluated at function entry |
| `contract_loop_entry (E)` | `E` evaluated before the first loop iteration |
| `contract_result` | the return value. `contract_returns` only |
| `contract_pointer_offset (p)` | `p`'s offset within its own object |
| `contract_ssize_t` | a signed type wide enough to hold that offset |

### Frame locations

Memory a function is allowed to write. These go inside `contract_assigns (...)`.

| location | is |
|---|---|
| `contract_range (p, lo, hi)` | elements `[lo, hi)` of `p`, half open |
| `contract_object_whole (p)` | the entire object `p` points into |
| `contract_object_from (p)` | from `p` to the end of its object |
| `contract_locations (a, b)` | two locations at once; nest for three or more |

A bare lvalue is also a location, so `contract_assigns (i)` says the function
may write `i` and nothing else.

```c
contract_assigns (contract_locations(op, contract_locations(ip,
                    contract_range(dst, 0, n))))
```

### Loop clauses

Clauses that go on a loop instead of a function, between the header and the
body. With them CBMC proves the loop by induction rather than unwinding it, so
the proof holds for every input rather than up to a bound.

```c
while (i < n)
  contract_assigns   (contract_locations(i, contract_range(p, 0, n)))
  contract_invariant (i <= n)
  contract_decreases (n - i)
{ p[i] = 0; i++; }
```

`contract_ghost` marks a variable that exists only for an annotation, so it
does not trigger `-Wunused-variable` where the clauses are not checked:

```c
BYTE* const opStart contract_ghost = op;
```

### Roles

A role expands to several clauses. Anything a role says, you can write out by
hand.

**Watch the units.** `contract_reads`/`contract_writes` count **bytes**, like
`memcpy`. The `_n` forms count **elements** of a typed pointer.
`contract_range(p, lo, hi)` also counts **elements**, and is half open.

| role | expands to |
|---|---|
| `contract_reads (p, n)` | `contract_pre (p != 0)`, `contract_pre (contract_readable(p, n))` |
| `contract_writes (p, n)` | `contract_pre (p != 0)`, `contract_pre (contract_writable(p, n))`, and a frame of `n` bytes at `p` |
| `contract_reads_n (p, n)` | as `contract_reads`, over `n * sizeof(*p)` bytes |
| `contract_writes_n (p, n)` | as `contract_writes`, over `n * sizeof(*p)` bytes |

A function that reads then writes carries both roles.

## Gotchas

- **Write `0`, not `NULL`.** `contract_pre (p != NULL)` does not compile on
  clang. `NULL` is `((void *)0)` and a cast to a pointer is not a constant
  expression in C. `contract_pre (p != 0)` works.
- **`contract_writes` says nothing about aliasing.** Two roles on one call may
  name the same buffer. Say `contract_pre (contract_disjoint(a, b))` or
  `contract_pre (contract_fresh(p, n))` if you mean it.

## Tests

Nothing to build first — the product is a header and two shell scripts, so the
gates are the whole of CI. `test/run.sh` runs them all; each one also runs on
its own. A gate whose prerequisite is missing skips loudly rather than failing.

| gate | needs |
|---|---|
| `test/header.sh` | a C compiler |
| `test/readme.sh` | a C compiler. Compiles this file's examples |
| `test/prove.sh` | CBMC. Runs `prove.sh` over `test/prove/` |
| `test/zstd.sh` | CBMC and a zstd checkout carrying the annotations |
