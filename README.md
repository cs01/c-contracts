# c-contracts

Contracts for C in one header you copy into your project. Clauses go on the
declaration, your compiler type-checks them, and CBMC proves them for every
input.

```c
#include "c_contracts.h"

int divide(int a, int b)
  contract_pre (b != 0)
{
  return a / b;
}
```

The clause goes on the declaration. Clang type-checks it at every call site via
`diagnose_if`; CBMC proves it for every input. GCC, MSVC and tcc preprocess it
away to the bare declaration.

```
$ c-contracts prove divide demo.c
divide: VERIFICATION SUCCESSFUL
```

That is a proof, not a test. What this project adds: the same file stays
ordinary C.

A fuller example, with memory, loops, and a frame:

```c
#include <stddef.h>
#include "c_contracts.h"

void zero(unsigned char *p, size_t n)
  contract_pre     (n > 0 && n < 64)
  contract_pre     (contract_fresh(p, n))
  contract_assigns (contract_range(p, 0, n))
{
  size_t i = 0;
  while (i < n)
    contract_assigns   (contract_locations(i, contract_range(p, 0, n)))
    contract_invariant (i <= n)
    contract_decreases (n - i)
  { p[i] = 0; i++; }
}
```

`contract_fresh(p, n)` says `p` is an object of exactly `n` bytes that nothing
else aliases. `contract_assigns` names what the function may write.
`contract_invariant` and `contract_decreases` let CBMC prove the loop by
induction rather than unwinding it, so the proof holds for every `n`, not up to
a bound.

```sh
curl -O https://raw.githubusercontent.com/cs01/c-contracts/main/include/c_contracts.h
```

Copy it into your tree and commit it. It is C89, has no `#include` of its own,
and defines `C_CONTRACTS_VERSION` so you can tell which copy you have:

```c
#if C_CONTRACTS_VERSION < 3
#error "c_contracts.h is too old"
#endif
```

`c-contracts` reports a header older than the one it was built against, rather
than silently finding fewer clauses.

Against zstd's decoder this proves `ZSTD_wildcopy` memory-safe for every length
in two seconds, and on its first run found undefined behaviour: `(BYTE*)dst -
(const BYTE*)src` computed before the branch that guards the only case where
both pointers are in the same object.

## Why not write `__CPROVER_requires` directly

Because it breaks every build that is not CBMC. Projects work around this with
private macro layers (AWS s2n has one, aws-c-common has a different one), and
CBMC's own docs note that repositories "may use their own names for some of
them." This is that layer as one vendorable file.

The problem with those wrappers is that they expand to **nothing** outside a CBMC
build. The spec becomes unparsed text between proof runs. A renamed field, a
stale bound, a typo: all invisible until someone runs CBMC, which most projects
rarely do. Here the `#else` branch is `diagnose_if`, so an ordinary
compile type-checks every clause in the function's own scope.

That is what tier 1 is for. Spec hygiene, not bug finding.

## How far each tier actually sees

Measured on `void sink(int n) contract_pre (n > 0);` with `enum { ZERO = 0 };`
and `opaque()` an extern function.

| call site | `clang -c` | `+ check` | `+ prove` |
|---|---|---|---|
| `sink(0)` | warns | warns | proves |
| `sink(ZERO)` enum constant | warns | warns | proves |
| `sink(1 - 1)` | warns | warns | proves |
| `const int n = 0; sink(n)` | warns | warns | proves |
| `int n = 0; sink(n)` | silent | warns | proves |
| `int n = 0; if (opaque()) n = 5; sink(n)` | silent | silent | proves |
| `sink(opaque())` | silent | silent | proves |

The compiler folds constants. `check` adds an intra-procedural dataflow pass, so
it sees through a variable but only keeps facts that every path agrees on. Neither is
a solver. Only the last column is a guarantee.

```
$ c-contracts check <file> -- <your compile flags>
```

| flag | does |
|---|---|
| `--list` | print every clause it found, with where you wrote it |
| `--warnings-as-errors` | exit non-zero on any contract problem, for CI |

`check` also type-checks `contract_returns (...)`, which needs the return type
bound to a name and so cannot ride on `diagnose_if`.

## Proving

```
$ c-contracts prove <function> <file> -- <your compile flags>
```

Needs CBMC 6+. Nothing here understands contracts: preprocessing the same source
with `-DC_CONTRACTS_CPROVER` is the whole lowering, and the rest is `goto-cc`,
`goto-instrument` and `cbmc`.

The entry point is generated from the preconditions, so you don't need a separate
harness full of hand-written `__CPROVER_assume` that drifts from the actual function. `contract_fresh(p, n)` allocates; every other conjunct is assumed;
a parameter no clause mentions stays nondeterministic.
`proofs/<function>.proof.c` overrides it where a project's allocation shape must
be written by hand.

| flag | does |
|---|---|
| `--mode=auto` | check the frame where possible, else fall back and say which loop has no contract |
| `--caller=f` | verify `f` against the callee's contract, the only mode where a precondition is an obligation |
| `--bound n=N` | cap a size the contract leaves open |
| `--unwind=N` | unwind bound, with unwinding assertions on |
| `--no-vacuity` | skip the check that the preconditions are satisfiable at all |
| `--solver=X` | pin one solver instead of racing them |
| `--timeout=N` | seconds any one step may take. Default 900 |
| `--proof-dir=D` | where `<function>.proof.c` may override the generated entry point |
| `--cbmc-flag=X` | passed straight through to `cbmc` |
| `--cc=X` | the preprocessor that lowers the clauses. Default `cc` |
| `--verbose` | print what CBMC printed |
| `--keep-work` | keep the working directory and say where it is |

Three things it does that a shell script around CBMC does not:

- `contract_writes (p, n)` with no `contract_fresh (p, n)` beside it gets
  reported before a solver runs, naming the pointer and the size. Without this
  CBMC reports ten failures that are not defects.
- The preconditions are checked for satisfiability by running the same
  assumptions and asserting false. A clean run means no input satisfies the
  preconditions and the proof is vacuous.
- The solvers race. CBMC 6.11 with `--z3` aborts on some loop-contract binaries,
  and solve time varies up to 20x between backends in either direction.

One caveat before trusting a frame: CBMC lets a loop's own `contract_assigns`
widen the function's frame inside that loop, and does not check that the loop's
targets lie within the function's.

## Reference

### Clauses

A clause is the specification. It goes after the parameter list, before the `;`
or `{`, and they stack. Everything else below is either shorthand for clauses or
vocabulary you use inside one.

| clause | means | checked by |
|---|---|---|
| `contract_pre (P)` | caller must establish `P` | compiler, tool, CBMC |
| `contract_post (P)` | `P` holds on return | compiler |
| `contract_returns (P)` | `P` holds on return, may name `contract_result` | tool, CBMC |
| `contract_assigns (L)` | nothing outside `L` changes | CBMC |
| `contract_writes_nothing()` | the frame is empty | CBMC |

### Roles

A role is one word that expands to several clauses. Anything a role says, you can
write out by hand.

Whether they help depends on what you are annotating. Over zstd:

| function | roles | primitives |
|---|---|---|
| `HUF_readStats` | 5 | 0 |
| `FSE_readNCount` | 5 | 0 |
| `HUF_decompress1X_usingDTable` | 2 | 0 |
| `BIT_initDStream` | 2 | 1 |
| `ZSTD_overlapCopy8` | 1 | 5 |
| `ZSTD_wildcopy`, `ZSTD_safecopy`, `ZSTD_execSequence` | 0 | 23 |

API boundaries where a parameter is a `(buffer, capacity)` pair: a role usually
says the whole thing. Hot-path internals with interior pointers and over-copy
slack: roles cover the boring part and you write the rest by hand.

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

### Predicates

A predicate is true or false and goes inside a clause. It is the vocabulary for
talking about memory, which C has no syntax for.
`contract_pre (contract_fresh(p, n))` is a clause containing a predicate;
`contract_fresh(p, n)` on its own specifies nothing.

| predicate | true when |
|---|---|
| `contract_readable (p, n)` | `n` bytes at `p` may be read |
| `contract_writable (p, n)` | `n` bytes at `p` may be written |
| `contract_fresh (p, n)` | `p` is an object of exactly `n` bytes that nothing else visible aliases |
| `contract_same_object (p, q)` | `p` and `q` point into one object |
| `contract_disjoint (p, q)` | they do not |
| `contract_forall (i, lo, hi, P)` | `P` holds for every `i` in `[lo, hi)` |

### Values

Things you can name inside a clause but that are not true or false. They appear
in ordinary C expressions, next to your own variables.

| value | is |
|---|---|
| `contract_old (E)` | `E` evaluated at function entry |
| `contract_result` | the return value. `contract_returns` only |
| `contract_pointer_offset (p)` | `p`'s offset within its own object |
| `contract_ssize_t` | a signed type wide enough to hold that offset |

### Frame locations

Memory a function is allowed to write. These go inside
`contract_assigns (...)` and nowhere else.

| location | is |
|---|---|
| `contract_range (p, lo, hi)` | elements `[lo, hi)` of `p`, half open |
| `contract_locations (a, b)` | two locations at once; nest for three or more |

A bare lvalue is also a location, so `contract_assigns (i)` says the function
may write `i` and nothing else. Nesting is how you get past two, because the
header uses no variadic macros and so stays valid C89:

```c
contract_assigns (contract_locations(op, contract_locations(ip,
                    contract_range(dst, 0, n))))
```

### Loop clauses

Clauses that go on a loop instead of a function, between the loop header and the
body. With them CBMC proves the loop by induction rather than unwinding it, which
is what makes a proof hold for every input rather than up to a bound.

```c
while (i < n)
  contract_assigns   (contract_locations(i, contract_range(p, 0, n)))
  contract_invariant (i <= n)
  contract_decreases (n - i)
{ p[i] = 0; i++; }
```

`contract_ghost` marks a variable that exists only for an annotation, so it does
not become `-Wunused-variable` where the clauses vanish:

```c
BYTE* const opStart contract_ghost = op;
```

## Things that will bite you

- **Write `0`, not `NULL`.** `contract_pre (p != NULL)` does not compile. `NULL`
  is `((void *)0)` and a cast to a pointer is not a constant expression in C, so
  the attribute is rejected. `contract_pre (p != 0)` works fine.
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

## Build the tool

Only if you want `check` or `prove`. The header needs none of this.

```sh
cmake -G Ninja -B build -DCMAKE_PREFIX_PATH=$(brew --prefix llvm)
ninja -C build
```

| gate | needs |
|---|---|
| `test/header.sh` | a C compiler |
| `test/readme.sh` | a C compiler. Compiles this file's examples |
| `test/run.sh` | the tool |
| `test/prove.sh` | `cbmc` 6+ |
| `test/differential.sh` | the reference clang fork |
| `test/zstd.sh` | a zstd checkout |

The last three skip when their prerequisite is missing.

## Status

The header is done, and it is what this repo adds: the proving is CBMC's.

`prove` is the least finished part, and it is also the part a user gets value
from, which is the wrong way round. It takes several files but only one function
name per run, and cannot find the annotated functions itself. It caches nothing,
so every run re-solves from scratch. It emits text and no report. Its generated
entry point discharges all seven proof fixtures, but the one real zstd function
needed a hand-written entry point.

`check` is in, and does less than the tier table above may suggest: it is spec
hygiene, not bug finding.

`docs/` has the working record and two clang defects this ran into, written up
ready to file.
