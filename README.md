# c-contracts

Contracts for C in one header. Write preconditions, postconditions, and frame
conditions on your functions. Clang type-checks them on every compile.
[CBMC](https://www.cprover.org/cbmc/) can prove them correct for all possible
inputs.

```c
#include "c_contracts.h"

int divide(int a, int b)
  contract_pre (b != 0)
{
  return a / b;
}
```

Clang warns at any call site where `b` might be zero:

```
$ clang -fsyntax-only demo.c
demo.c:8:5: warning: precondition b != 0 is violated by this call
    divide(10, 0);
    ^
```

CBMC proves no call can violate it, for every possible value of `a` and `b`:

```
$ ./prove.sh divide demo.c
divide: VERIFICATION SUCCESSFUL
```

That is a proof, not a test.

```sh
curl -O https://raw.githubusercontent.com/cs01/c-contracts/main/include/c_contracts.h
```

Copy it into your tree and commit it. The header is C89 with no includes of
its own. On GCC, MSVC, and tcc, every clause preprocesses away to the bare
declaration. On clang, `diagnose_if` type-checks each clause at every call
site. On CBMC, each clause becomes a proof obligation.

## What each level catches

Given `void sink(int n) contract_pre (n > 0);`:

| call site | compile | prove |
|---|---|---|
| `sink(0)` | warns | proves |
| `sink(ZERO)` (enum) | warns | proves |
| `const int n = 0; sink(n)` | warns | proves |
| `int n = 0; sink(n)` | silent | proves |
| `int n = 0; if (opaque()) n = 5; sink(n)` | silent | proves |
| `sink(opaque())` | silent | proves |

**Compile** folds constants and warns where clang can see a violation, with no
extra tooling. **Prove** hands the function to CBMC, which checks every
possible input. Only proving is a guarantee.

## Proving

No harness needed. The contract annotations are the spec.

```
$ ./prove.sh zero source.c -Iinclude
lowered 4 clause(s)
mode: enforce (frame checked)
...
VERIFICATION SUCCESSFUL
== solved by sat in 2s
```

`prove.sh` preprocesses the source to CBMC syntax, and CBMC generates the
entry point from the preconditions: `contract_fresh(p, n)` becomes an
allocation, other preconditions become assumptions, and the frame is checked
against `contract_assigns`. `solve.sh` races every installed solver and takes
the first clean answer, because solve time varies up to 20x between backends.

Modular verification: once you prove a dependency, use `-r` so callers
trust its contract instead of re-analyzing its body:

```
$ ./prove.sh compress_bound source.c           # prove the leaf
$ ./prove.sh compress source.c -r compress_bound  # prove the caller
```

This scales to large codebases. Each proof stays small regardless of the
call tree below it.

Needs [CBMC](https://www.cprover.org/cbmc/) 6+ (`goto-cc`,
`goto-instrument`, `cbmc`).

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

| gate | needs |
|---|---|
| `test/header.sh` | a C compiler |
| `test/readme.sh` | a C compiler. Compiles this file's examples |
