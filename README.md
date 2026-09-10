# c-contracts

Contracts for C that a released clang checks. No fork, no compiler patch, no
plugin.

`c_contracts.h` is a single vendorable header, written in C89, with no includes.
It expands to one of four targets chosen at include time. Under any clang with
`diagnose_if`, preconditions and postconditions are checked with no tooling
installed at all. `c-contracts` is an out-of-tree binary that adds what the
header cannot do alone: `returns`, the frame, a call-site dataflow pass, and
CBMC proofs.

```c
#define C_CONTRACTS_NO_PREFIX
#include <c_contracts.h>

int *allocate(size_t n) pre (n > 0);

size_t decode(void *dst, size_t dstCap, const void *src, size_t srcSize)
  writes  (dst, dstCap)
  reads   (src, srcSize)
  returns (c_result <= old(dstCap));
```

```
$ clang -std=c89 -Wall -c demo.c
demo.c:14:22: warning: precondition n > 0 is violated by this call
              [-Wuser-defined-warnings]
   14 |   int *p = allocate(0);
      |                      ^
```

The same file builds under GCC, MSVC and tcc, where every clause preprocesses
away to the bare declaration.

## Language

### Clauses

Written after the parameter list, before the `;` or the `{`. They stack, and a
contract split across a declaration and its definition is one contract.

| clause | means | checked by |
|---|---|---|
| `pre (P)` | caller must establish `P` | clang at every call site; the dataflow pass; CBMC |
| `post (P)` | `P` holds on return, naming no result | clang, in place |
| `returns (P)` | `P` holds on return, and may name `c_result` | this tool; CBMC |
| `assigns (L)` | nothing outside `L` is modified | CBMC only |
| `c_writes_nothing` | the frame is empty | CBMC only |

`P` is any C expression valid in the function's prototype scope. `L` is one or
more locations: an lvalue, a `range(...)`, or `locations(...)` of those.

### Roles

Shorthand for the clause pairs a buffer parameter almost always wants. Counts
are in **bytes**; the `_n` forms count **elements** of a typed pointer.

| role | expands to |
|---|---|
| `reads (p, n)` | `pre (p != 0)`, `pre (readable(p, n))` |
| `writes (p, n)` | `pre (p != 0)`, `pre (writable(p, n))`, `assigns (((char *)p)[0 : n])` |
| `reads_n (p, n)` | as `reads`, over `n * sizeof(*p)` bytes |
| `writes_n (p, n)` | as `writes`, over `n * sizeof(*p)` bytes, `assigns (p[0 : n])` |

There are two roles, not three. A function that reads a buffer and then writes
it carries both. `reads` is what obliges the caller to have initialized the
memory: `memset` only writes, `buf[i] *= 2` does both.

### Predicates

Usable inside any clause.

| predicate | means | CBMC |
|---|---|---|
| `readable (p, n)` | `n` bytes at `p` may be read | `__CPROVER_r_ok` |
| `writable (p, n)` | `n` bytes at `p` may be written | `__CPROVER_w_ok` |
| `fresh (p, n)` | an object of exactly `n` bytes that nothing else visible aliases | `__CPROVER_is_fresh` |
| `same_object (p, q)` | `p` and `q` point into one object | `__CPROVER_same_object` |
| `disjoint (p, q)` | they do not | `!__CPROVER_same_object` |
| `pointer_offset (p)` | `p`'s offset within its object | `__CPROVER_POINTER_OFFSET` |
| `old (E)` | `E` evaluated at function entry | `__CPROVER_old` |
| `c_result` | the return value; `returns` only | `__CPROVER_return_value` |
| `range (p, lo, hi)` | elements `[lo, hi)` of `p`, for a frame | `__CPROVER_object_upto` |
| `locations (a, b)` | two frame locations; nests for more | `a, b` |
| `c_forall (i, lo, hi, P)` | `P` for every `i` in `[lo, hi)` | `__CPROVER_forall` |
| `c_ssize_t` | a signed type wide enough for a pointer offset | `__CPROVER_ssize_t` |

### Loop clauses

Written between the loop header and its body. CBMC discharges the loop by
induction instead of unwinding it, which is what makes a proof unbounded.

| clause | means |
|---|---|
| `assigns (L)` | the loop modifies nothing outside `L` |
| `loop_invariant (P)` | `P` holds on entry and after every iteration |
| `decreases (M)` | `M` strictly decreases and is bounded below |

```c
while (i < n)
  assigns        (locations(i, range(p, 0, n)))
  loop_invariant (i <= n)
  decreases      (n - i)
{ p[i] = 0; i++; }
```

### Ghost declarations

`c_ghost` marks a variable that exists only to be named by an annotation, so it
does not become `-Wunused-variable` in a build where the clauses vanish:

```c
BYTE* const opStart c_ghost = op;
```

## Rules

- **Two spellings.** Every name above has a `c_` form (`c_pre`, `c_fresh`,
  `c_range`) that always works. The unprefixed forms shown in the tables need
  `#define C_CONTRACTS_NO_PREFIX` before the include, and are opt-in because
  they take common words.
- **Only function-like names are unprefixed.** A macro that is function-like
  expands only where its name is followed by `(`, so a project keeps `int pre;`,
  `s.post` and a field called `range`. The object-like names are therefore never
  unprefixed: write `c_result`, `c_ssize_t`, `c_ghost`, `c_writes_nothing`.
  Check a candidate project by grepping for `name(`, not for the bare word.
- **No unprefixed `forall`.** Under a contract-aware front end it binds its
  variable with its own syntax; the portable form takes four arguments. One name
  cannot serve both, so write `c_forall`.
- **Spell the null pointer `0`.** `pre (p != NULL)` does not compile: `NULL` is
  `((void *)0)`, and a cast to a pointer is not a constant expression in C, so
  `diagnose_if` rejects the clause. `pre (p != 0)` and `pre (!p)` are fine. See
  `docs/clang-diagnose_if-null-constant.md`.
- **`writes` says nothing about aliasing.** Two roles on one call may name the
  same buffer. A function that needs two buffers not to overlap says
  `pre (disjoint(a, b))`; a function that needs a whole object to itself says
  `pre (fresh(p, n))`. This is not decorative: `writes` alone lowers to `w_ok`,
  which does not discharge a proof of even the simplest buffer-writing function.
  Both are object level, so two non-overlapping ranges inside one object are not
  disjoint.
- **A clause is quoted as written.** Project macros inside it are not expanded
  before the tool sees them, so they mean what they mean in the translation unit
  they came from.

## Targets

| target | selected when | `pre` | `post` | `returns` | frame, loops |
|---|---|---|---|---|---|
| stock clang | `__has_attribute(diagnose_if)` | call-site warning | type-checked in place | quoted for the tool | dropped |
| CBMC | `-DC_CONTRACTS_CPROVER` | `__CPROVER_requires` | `__CPROVER_ensures` | `__CPROVER_ensures` | `__CPROVER_assigns`, `__CPROVER_loop_invariant` |
| contract-aware front end | `__has_feature(c_contracts)` | grammar | grammar | grammar | grammar |
| everything else | otherwise | nothing | nothing | nothing | nothing |

`-DC_CONTRACTS_STOCK=0` forces the last row from a clang, for a build that wants
the annotations present and inert.

`diagnose_if` is what makes the first row work: its argument is parsed in the
function's own prototype scope, so a precondition gets name lookup, type checking
and call-site folding from a compiler that has never heard of contracts. `post`
rides the same mechanism with its condition folded to false, so it never fires
and is type-checked anyway.

`returns` cannot. Binding a name to the function's own return type needs a
declaration, so it needs a statement expression, and a statement expression
inside a late-parsed attribute argument crashes clang (22.1.8 and trunk, see
`docs/clang-diagnose_if-stmtexpr-crash.md`). So `returns` and the frame ride as
`annotate` strings, which clang lexes and never parses. That is what the tool
reads.

## The tool

```
$ c-contracts check demo.c -- -std=c89 -Iinclude
demo.c:11:3: error: use of undeclared identifier 'dstCapp'
demo.c:19:3: warning: precondition n > 0 of 'allocate' is violated by this call
```

Two things beyond what clang alone gives:

**`returns` and the frame are type-checked** by synthesizing a function with the
same parameter list and a local bound to the return type via `__typeof__`,
appending it to the translation unit and reparsing, so the clause is checked with
every typedef and macro it was written against still in force.

**A call-site dataflow pass.** Clang's own `diagnose_if` fires only when the
condition folds against the argument expressions, so `int n = 0; allocate(n);`
reaches nobody. The pass is a CFG dataflow to a fixpoint that catches it.

## Proving

```
$ c-contracts prove zero demo.c -- -std=c89 -Iinclude
lowered 6 clause(s):
  __CPROVER_requires(n > 0 && n < 64)
  __CPROVER_requires(__CPROVER_is_fresh((p), (n)))
  ...
mode: enforce (frame checked)
vacuity: zero's preconditions are satisfiable
solved by sat
zero: VERIFICATION SUCCESSFUL
```

Nothing in the pipeline understands contracts: preprocessing the same source
with `-DC_CONTRACTS_CPROVER` is the whole lowering, and the rest is `goto-cc`,
`goto-instrument` and `cbmc`.

The entry point is generated from the preconditions. `fresh(p, n)` allocates,
every other conjunct is assumed, and a parameter no clause mentions is left
nondeterministic. `proofs/<function>.proof.c` on disk overrides it.

| flag | does |
|---|---|
| `--mode=auto` | check the frame where possible, else fall back and say which loop has no contract. `enforce` and `harness` pin one |
| `--caller=f` | verify `f` against the callee's contract, the only mode where a precondition is an obligation |
| `--bound n=N` | cap a size the contract leaves open |
| `--unwind=N` | CBMC's unwind bound, with unwinding assertions on |
| `--no-vacuity` | skip the check that the preconditions are satisfiable at all |
| `--solver=` | pin one instead of racing them |
| `--proof-dir=` | where a hand-written entry point may live |

Three behaviours worth knowing about:

- `writes (p, n)` with no `fresh (p, n)` beside it is reported before a solver
  runs, naming the pointer and the size to put in the missing clause.
- The preconditions are checked for satisfiability by running the same
  assumptions and asserting false. A clean run there means nothing can call the
  function and the proof proved nothing.
- The solvers race. CBMC 6.11 with `--z3` aborts on some loop-contract binaries,
  and COST.md in the reference tree measures up to 20x between backends in either
  direction.

One caveat before trusting a frame: CBMC lets a loop's own `assigns` widen the
function's frame inside that loop, and does not check that the loop's targets lie
within the function's.

## Build

Needs an LLVM install that ships `ClangConfig.cmake`. Any released one.

```sh
cmake -G Ninja -B build -DCMAKE_PREFIX_PATH=$(brew --prefix llvm)
ninja -C build
```

| gate | holds | needs |
|---|---|---|
| `test/run.sh` | clause extraction, ghost checking, the call-site pass | nothing |
| `test/prove.sh` | the CBMC tier end to end, including the frame and vacuity gates | `cbmc` 6+ |
| `test/differential.sh` | this lowering against the reference implementation's, clause for clause | the fork, built |
| `test/zstd.sh` | both tiers on a codebase not written for them | a zstd checkout |

The last three skip when their prerequisite is missing.

## Status

`pre` and `post` work with no tool installed. `returns`, the frame, the
call-site pass and `prove` are the tool.

Against zstd's decoder it reads the contracts out of an ordinary parse and proves
`ZSTD_wildcopy` memory-safe for every length in two seconds. It found undefined
behaviour there on the first run: `(BYTE*)dst - (const BYTE*)src` computed before
the branch that is the only case where the two pointers are in one object.

The reference implementation is a clang fork that parses all of this as real
grammar. It is not shipped and not required; it is the differential oracle. One
thing it can do that this cannot: rewrite a `do`/`while` so a loop contract can
attach to it, which `goto-instrument` otherwise refuses. Out of tree that
restructuring is the author's.

`docs/worksheets/2026-09-09-port-from-fork.md` is the working record.
