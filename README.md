# c-contracts

Prove a C function correct for every input, from annotations that survive an
ordinary build.

```c
#include "c_contracts.h"

void zero(unsigned char *p, size_t n)
  contract_pre     (n > 0 && n < 64)
  contract_pre     (contract_fresh(p, n))
  contract_assigns (contract_range(p, 0, n))
{ ... }
```

```
$ c-contracts prove zero demo.c -- -Iinclude
mode: enforce (frame checked)
vacuity: zero's preconditions are satisfiable
zero: VERIFICATION SUCCESSFUL
```

That is a proof, not a test: it holds for every `p` and every `n` the
preconditions allow. CBMC does the proving. What this adds is that the same
annotated file still compiles under GCC, MSVC, tcc and any clang, where every
clause disappears.

```sh
curl -O https://raw.githubusercontent.com/cs01/c-contracts/main/include/c_contracts.h
```

Against zstd's decoder this proves `ZSTD_wildcopy` memory-safe for every length
in two seconds, and found undefined behaviour on the first run: `(BYTE*)dst -
(const BYTE*)src` computed before the branch that is the only case where the two
pointers are in one object.

## Why not write `__CPROVER_requires` directly

Because it breaks every build that is not CBMC, so projects wrap it in a private
macro layer: AWS s2n has one, aws-c-common has a different one, and CBMC's own
docs note that repositories "may use their own names for some of them". This is
that layer as one vendorable file.

Their wrapper expands to **nothing** outside a CBMC build, which means the spec
is unparsed text between proof runs. A renamed field, a stale bound, a typo: all
invisible until someone runs CBMC, which for most projects is rarely. Here the
`#else` branch is `diagnose_if`, so an ordinary compile type-checks every clause
in the function's own scope.

That is what tier 1 is for. It is spec hygiene, not bug finding.

## How far each tier actually sees

Measured, on `void sink(int n) contract_pre (n > 0);`

| call site | `clang -c` | `+ check` | `+ prove` |
|---|---|---|---|
| `sink(0)` | warns | warns | proves |
| `sink(ZERO)` enum constant | warns | warns | proves |
| `sink(1 - 1)` | warns | warns | proves |
| `const int n = 0; sink(n)` | warns | warns | proves |
| `int n = 0; sink(n)` | silent | warns | proves |
| `int n = 0; if (c) n = 5; sink(n)` | silent | silent | proves |
| `sink(opaque())` | silent | silent | proves |

The compiler folds constants. `check` adds an intra-procedural dataflow pass, so
it sees through a variable but only keeps facts every path agrees on. Neither is
a solver. Only the last column is a guarantee.

`check` also type-checks `contract_returns (...)`, which needs the return type
bound to a name and so cannot ride on `diagnose_if`.

## Proving

```
$ c-contracts prove <function> <file> -- <your compile flags>
```

Needs CBMC 6+. Nothing in the pipeline understands contracts: preprocessing the
same source with `-DC_CONTRACTS_CPROVER` is the whole lowering, and the rest is
`goto-cc`, `goto-instrument` and `cbmc`.

The entry point is generated from the preconditions, so there is no separate
harness full of hand-written `__CPROVER_assume` that nothing keeps in step with
the function. `contract_fresh(p, n)` allocates; every other conjunct is assumed;
a parameter no clause mentions stays nondeterministic.
`proofs/<function>.proof.c` overrides it where a project's allocation shape has
to be said by hand.

| flag | does |
|---|---|
| `--mode=auto` | check the frame where possible, else fall back and say which loop has no contract |
| `--caller=f` | verify `f` against the callee's contract, the only mode where a precondition is an obligation |
| `--bound n=N` | cap a size the contract leaves open |
| `--unwind=N` | unwind bound, with unwinding assertions on |
| `--no-vacuity` | skip the check that the preconditions are satisfiable at all |

Three things it does that a shell script around CBMC does not:

- `contract_writes (p, n)` with no `contract_fresh (p, n)` beside it is reported
  before a solver runs, naming the pointer and the size for the missing clause.
  Without it CBMC reports ten failures that are not defects.
- The preconditions are checked for satisfiability by running the same
  assumptions and asserting false. A clean run there means nothing can call the
  function and the proof proved nothing.
- The solvers race. CBMC 6.11 with `--z3` aborts on some loop-contract binaries,
  and solve time varies up to 20x between backends in either direction.

One caveat before trusting a frame: CBMC lets a loop's own `contract_assigns`
widen the function's frame inside that loop, and does not check that the loop's
targets lie within the function's.

## Reference

### Clauses

After the parameter list, before the `;` or the `{`. They stack.

| clause | means | checked by |
|---|---|---|
| `pre (P)` | caller must establish `P` | compiler, tool, CBMC |
| `post (P)` | `P` holds on return | compiler |
| `returns (P)` | `P` holds on return, may name `contract_result` | tool, CBMC |
| `assigns (L)` | nothing outside `L` changes | CBMC |
| `contract_writes_nothing` | the frame is empty | CBMC |

### Roles

What a function does to a buffer. Counts are **bytes**; `_n` forms count
**elements**.

| role | expands to |
|---|---|
| `reads (p, n)` | `pre (p != 0)`, `pre (readable(p, n))` |
| `writes (p, n)` | `pre (p != 0)`, `pre (writable(p, n))`, `assigns (((char *)p)[0 : n])` |
| `reads_n (p, n)` | as `reads`, over `n * sizeof(*p)` bytes |
| `writes_n (p, n)` | as `writes`, over `n * sizeof(*p)` bytes |

Two roles, not three. A function that reads then writes carries both. `reads` is
what says the caller must have initialized the memory.

### Predicates

| predicate | means |
|---|---|
| `readable (p, n)` | `n` bytes at `p` may be read |
| `writable (p, n)` | `n` bytes at `p` may be written |
| `fresh (p, n)` | an object of exactly `n` bytes that nothing else visible aliases |
| `same_object (p, q)` | `p` and `q` point into one object |
| `disjoint (p, q)` | they do not |
| `pointer_offset (p)` | `p`'s offset within its object |
| `old (E)` | `E` at function entry |
| `contract_result` | the return value; `returns` only |
| `range (p, lo, hi)` | elements `[lo, hi)` of `p`, for a frame |
| `locations (a, b)` | two frame locations; nests for more |
| `contract_forall (i, lo, hi, P)` | `P` for every `i` in `[lo, hi)` |
| `contract_ssize_t` | signed, wide enough for a pointer offset |

### Loops

Between the loop header and the body. CBMC then proves the loop by induction
instead of unwinding it, which is what makes a proof unbounded.

```c
while (i < n)
  assigns        (locations(i, range(p, 0, n)))
  loop_invariant (i <= n)
  decreases      (n - i)
{ p[i] = 0; i++; }
```

`contract_ghost` marks a variable that exists only for an annotation, so it does not
become `-Wunused-variable` where the clauses vanish:

```c
BYTE* const opStart contract_ghost = op;
```

## Things that will bite you

- **Write `0`, not `NULL`.** `pre (p != NULL)` does not compile. `NULL` is
  `((void *)0)` and a cast to a pointer is not a constant expression in C, so
  the attribute is rejected. `pre (p != 0)` and `pre (!p)` are fine.
- **One spelling, always prefixed.** A vendored header may not take words as
  common as `pre`, `range` or `result` out of a project's namespace, and an
  opt-in second spelling only moves that decision to whoever includes the file
  first.
- **`writes` says nothing about aliasing.** Two roles on one call may name the
  same buffer. Say `pre (disjoint(a, b))` or `pre (fresh(p, n))` if you mean it.
  It is not decorative: `writes` alone will not discharge a proof of even the
  simplest buffer-writing function.

## Targets

The header picks one at include time.

| target | when | `pre` | frame, loops |
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
| `test/run.sh` | the tool |
| `test/prove.sh` | `cbmc` 6+ |
| `test/differential.sh` | the reference clang fork |
| `test/zstd.sh` | a zstd checkout |

The last three skip when their prerequisite is missing.

## Status

`prove` is where the value is and it is the least finished part: no
project-level runs, no caching, no proof reports, and a generated entry point
that handles the easy cases and falls back to a hand-written one otherwise. That
ordering is the current problem with this repo, not a description of a plan.

The header is done. `check` is in.

`docs/` has the working record and two clang defects this ran into, written up
ready to file.
