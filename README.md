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

Clang warns at any call site where `b` might be zero. CBMC proves no call can
violate it, for every possible value of `a` and `b`:

```
$ ./prove.sh divide demo.c
divide: VERIFICATION SUCCESSFUL
```

That is a proof, not a test.

```sh
curl -O https://raw.githubusercontent.com/cs01/c-contracts/main/include/c_contracts.h
```

Copy it into your tree and commit it. The header is C89 with no includes of
its own. On compilers without `diagnose_if` (GCC, MSVC, tcc), every clause
preprocesses away to the bare declaration.

## Why a wrapper

CBMC has its own contract syntax (`__CPROVER_requires`, etc.), but it breaks
every compiler that is not CBMC. Projects work around this with private macro
layers (AWS s2n has one, aws-c-common has a different one). This is that layer
as one vendorable file.

The problem with those wrappers: they expand to **nothing** outside CBMC. The
spec becomes unparsed text between proof runs. A renamed field, a stale bound,
a typo, all invisible until someone runs CBMC, which most projects rarely do.
Here the fallback is `diagnose_if`, so an ordinary compile type-checks every
clause in the function's own scope.

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

Write a harness that calls the function with nondeterministic inputs:

```c
// harness.c — includes the source, calls the function with nondeterministic inputs
void harness(void)
{
  size_t n;
  __CPROVER_assume(n > 0 && n < 64);
  unsigned char *p = __CPROVER_allocate(n, 0);
  zero(p, n);
}
```

Then prove it:

```
$ ./prove.sh zero harness.c -Iinclude
lowered 4 contract clause(s)
mode: enforce (frame checked)
...
VERIFICATION SUCCESSFUL
== solved by sat in 2s
```

Needs [CBMC](https://www.cprover.org/cbmc/) 6+ (`goto-cc`, `goto-instrument`,
`cbmc`). `prove.sh` preprocesses with `-DC_CONTRACTS_CPROVER` to lower the
macros to CBMC's syntax, compiles to a goto program, applies loop contracts,
enforces the function's frame, and runs `cbmc`. `solve.sh` races every
installed solver (MiniSat, z3, bitwuzla, cvc5) and takes the first clean
answer, because solve time varies up to 20x between backends.

One caveat: CBMC lets a loop's `contract_assigns` widen the function's frame
inside that loop, and does not check that the loop's targets lie within the
function's.

## Reference

### Clauses

A clause goes after the parameter list, before the `;` or `{`. They stack.
Everything else below is shorthand for clauses or vocabulary you use inside one.

| clause | means | checked by |
|---|---|---|
| `contract_pre (P)` | caller must establish `P` | compile, prove |
| `contract_post (P)` | `P` holds on return | compile |
| `contract_returns (P)` | `P` holds on return, may name `contract_result` | prove |
| `contract_assigns (L)` | nothing outside `L` changes | prove |
| `contract_writes_nothing()` | the frame is empty | prove |

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
| `contract_forall (i, lo, hi, P)` | `P` holds for every `i` in `[lo, hi)` |

### Values

Things you can name inside a clause that are not true or false.

| value | is |
|---|---|
| `contract_old (E)` | `E` evaluated at function entry |
| `contract_result` | the return value. `contract_returns` only |
| `contract_pointer_offset (p)` | `p`'s offset within its own object |
| `contract_ssize_t` | a signed type wide enough to hold that offset |

### Frame locations

Memory a function is allowed to write. These go inside `contract_assigns (...)`.

| location | is |
|---|---|
| `contract_range (p, lo, hi)` | elements `[lo, hi)` of `p`, half open |
| `contract_locations (a, b)` | two locations at once; nest for three or more |

A bare lvalue is also a location, so `contract_assigns (i)` says the function
may write `i` and nothing else. Nesting is how you combine more than two,
because the header uses no variadic macros and so stays valid C89:

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
does not trigger `-Wunused-variable` where the clauses vanish:

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

Two roles, not three. A function that reads then writes carries both.
`contract_reads` is what says the caller must have initialized the memory.

## Gotchas

- **Write `0`, not `NULL`.** `contract_pre (p != NULL)` does not compile.
  `NULL` is `((void *)0)` and a cast to a pointer is not a constant expression
  in C, so the attribute is rejected. `contract_pre (p != 0)` works.
- **One spelling, always prefixed.** A vendored header should not take words as
  common as `pre`, `range` or `result` out of a project's namespace.
- **`contract_writes` says nothing about aliasing.** Two roles on one call may
  name the same buffer. Say `contract_pre (contract_disjoint(a, b))` or
  `contract_pre (contract_fresh(p, n))` if you mean it. Without it,
  `contract_writes` alone will not discharge a proof of even the simplest
  buffer-writing function.

## Targets

The header picks one at include time.

| target | when | `contract_pre` | frame, loops |
|---|---|---|---|
| stock clang | `__has_attribute(diagnose_if)` | call-site warning | dropped |
| CBMC | `-DC_CONTRACTS_CPROVER` | `__CPROVER_requires` | `__CPROVER_assigns` etc. |
| contract-aware front end | `__has_feature(c_contracts)` | grammar | grammar |
| anything else | otherwise | nothing | nothing |

`-DC_CONTRACTS_STOCK=0` forces the last row, for a build that wants the
annotations present and inert.

## Tests

| gate | needs |
|---|---|
| `test/header.sh` | a C compiler |
| `test/readme.sh` | a C compiler. Compiles this file's examples |

## Status

The header is the project. `prove.sh` and `solve.sh` are the proving workflow.
No build step, no compiled binary, no LLVM dependency. Copy the header, write a
harness, run the script.
