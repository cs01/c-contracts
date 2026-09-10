# c-contracts

Contracts for C, checked by a released clang. No fork, no compiler patch, no
plugin.

```c
#define C_CONTRACTS_NO_PREFIX
#include <c_contracts.h>

int *allocate(size_t n) pre (n > 0);

size_t decode(void *dst, size_t dstCap, const void *src, size_t srcSize)
  writes  (dst, dstCap)
  reads   (src, srcSize)
  returns (c_result <= old(dstCap));
```

The same source builds under GCC, MSVC and tcc, where every clause preprocesses
away to the bare declaration. Under any clang new enough to have `diagnose_if`,
preconditions become real checks with no tooling at all:

```
$ clang -std=c89 -Wall -c demo.c
demo.c:14:22: warning: precondition n > 0 is violated by this call
              [-Wuser-defined-warnings]
   14 |   int *p = allocate(0);
      |                      ^
demo.c:6:25: note: from 'diagnose_if' attribute on 'allocate':
```

## How it works

`c_contracts.h` picks a target at include time. Three of the four need nothing
installed.

| target | selected when | preconditions | frames, loop contracts |
|---|---|---|---|
| stock clang | `__has_attribute(diagnose_if)` | `diagnose_if`, checked at every call site | dropped |
| CBMC | `-DC_CONTRACTS_CPROVER` | `__CPROVER_requires` | `__CPROVER_assigns`, `__CPROVER_loop_invariant` |
| contract-aware front end | `__has_feature(c_contracts)` | grammar | grammar |
| everything else | otherwise | nothing | nothing |

`diagnose_if` is the whole trick. Its argument is parsed in the function's own
prototype scope, so a precondition gets name lookup, type checking and
call-site folding from a compiler that has never heard of contracts.

That covers `post` too, which is why most postconditions need no tool either. A
`post` is a result-independent fact, so it names nothing that is not already in
scope where it is written; the header hands it to `diagnose_if` with the
condition folded to false, so it never fires and clang type-checks it anyway.

`returns` is the one that cannot work this way. Binding a name to the function's
own return type needs a declaration, so it needs a statement expression, and a
statement expression inside a late-parsed attribute argument crashes clang
(22.1.8 and trunk). So the header leaves `returns`, and the frame, quoted in an
`annotate` string, which clang lexes but never parses. That is what this tool
reads. For each one it synthesizes a function with the same parameter
list and a local bound to the return type via `__typeof__`, appends it to the
translation unit, and reparses -- so the clause is checked with every typedef and
macro it was written against still in force.

### What `writes` does not say

`writes (p, n)` says the memory is valid to write. It says nothing about
aliasing: two roles on one call may name the same buffer. A function that needs
two buffers not to overlap has to say which two, and that clause is what makes
it provable:

```c
void copy(void *dst, const void *src, size_t n)
  writes (dst, n)
  reads  (src, n)
  pre    (disjoint(dst, src));
```

`fresh (p, n)` is the single-buffer form: an object of exactly `n` bytes that
nothing else the call can see aliases. The distinction is not decoration.
`writes` alone lowers to CBMC's `w_ok`, which does not discharge a proof of even
the simplest buffer-writing function; `fresh` lowers to `is_fresh`, which does.
Both `disjoint` and `fresh` are object level, the granularity `is_fresh` works
at, so two non-overlapping ranges inside one object are not disjoint by this
definition.

```
$ c-contracts check src/decode.c -- -std=c11 -Iinclude
src/decode.c:11:3: error: use of undeclared identifier 'dstCapp'; did you mean 'dstCap'?
```

## Build

Needs an LLVM install that ships `ClangConfig.cmake` -- any released one.

```sh
cmake -G Ninja -B build -DCMAKE_PREFIX_PATH=$(brew --prefix llvm)
ninja -C build
test/run.sh build/c-contracts
```

## Proving it

`c-contracts prove` is the third tier: not "is this contract well formed" and
not "does this caller break it", but "is it true for every input". CBMC answers
that, and nothing in the pipeline understands contracts -- preprocessing the
same source with `-DC_CONTRACTS_CPROVER` is the whole lowering.

```
$ c-contracts prove zero demo.c -- -std=c89 -Iinclude
lowered 6 clause(s):
  __CPROVER_requires(n > 0 && n < 64)
  __CPROVER_requires(__CPROVER_is_fresh((p), (n)))
  __CPROVER_assigns(__CPROVER_object_upto((p) + (0), ((n) - (0)) * sizeof(*(p))))
  ...
mode: enforce (frame checked)
vacuity: zero's preconditions are satisfiable
solved by sat
zero: VERIFICATION SUCCESSFUL
```

The entry point is generated from the preconditions -- `fresh(p, n)` allocates,
everything else is assumed -- so there is no hand-written `__CPROVER_assume`
encoding a claim nobody reviews. `proofs/<function>.proof.c` on disk wins over
the generated one where a project's allocation shape needs saying by hand.

Four things it does that a shell script around CBMC does not:

- **It names the shape that cannot discharge.** `writes (p, n)` with no
  `fresh (p, n)` beside it produces a warning naming the missing clause, before
  a solver runs. Without it the user sees ten failures and goes looking for a
  bug in their own code.
- **It checks that the preconditions are satisfiable at all**, by running the
  same assumptions with `assert(0)` after them: reachable by construction, so a
  clean run means nothing can call the function and the proof proved nothing.
  On by default; `--no-vacuity` opts out. It is cheap -- the probe stops before
  the call, so it never runs the function body.
- **It races the solvers** rather than picking one. COST.md in the reference
  tree measures up to 20x between CBMC's built-in SAT backend and an SMT solver,
  in *either* direction, and cbmc 6.11 with `--z3` aborts outright on some
  loop-contract binaries -- so a tool that picked one would report a crash where
  the other has a proof.
- **It has a caller mode.** `--caller=f` verifies `f` against the callee's
  contract, which is the only mode in which a precondition is an *obligation*
  rather than an assumption. `pre (disjoint(dst, src))` is invisible until then.

`--mode=auto` (the default) checks the frame where it can and falls back to the
generated entry point when a loop in the function carries no contract, saying
which. One caveat worth knowing before trusting a frame: CBMC lets a loop's own
`assigns` widen the function's frame inside that loop, and does not check that
the loop's targets lie within the function's.

## Status

Working: clause extraction, postcondition type checking, call-site precondition
warnings (from clang itself). `post` and `pre` are both checked by a stock clang
with no tool installed; `returns` and the frame are what the tool is for.

The call-site dataflow pass is in: a violation that travels through a variable,
which constant folding cannot see, is reported at the call site.

```
$ c-contracts check demo.c -- -std=c89 -Iinclude
demo.c:19:3: warning: precondition n > 0 of 'allocate' is violated by this call
```

`c-contracts prove` is in, and runs on real code. Against zstd's decoder it
reads the contracts out of an ordinary parse and proves `ZSTD_wildcopy`
memory-safe for *every* length in two seconds -- and found undefined behaviour
in it on the first run: `(BYTE*)dst - (const BYTE*)src` computed before the
branch that is the only case where the two pointers are in the same object.

## Gates

| gate | what it holds | prerequisite |
|---|---|---|
| `test/run.sh` | clause extraction, ghost checking, the call-site pass | none |
| `test/prove.sh` | the CBMC tier, end to end, including the frame and vacuity gates | `cbmc` 6+ |
| `test/differential.sh` | this tool's lowering against the fork's, clause for clause | the fork, built |
| `test/zstd.sh` | both tiers on a codebase not written for them | a zstd checkout |

Each skips loudly rather than failing when its prerequisite is missing: a suite
that cries wolf for reasons that have nothing to do with the code is a suite
nobody reads.

The reference implementation is a clang fork that parses all of this as real
grammar. It is not shipped and not required; it exists as the differential
oracle this tool is checked against.

`docs/worksheets/2026-09-09-port-from-fork.md` is the working record: what was
built, what the gates hold, the gotchas already paid for, and the decisions
behind the surface language.

## Known clang defects this ran into

Both are in `docs/`, written up ready to file, and both shape the header:

- a statement expression inside a late-parsed attribute argument segfaults the
  parser, which is why `returns` cannot be checked in place like `post`
  (`clang-diagnose_if-stmtexpr-crash.md`);
- `diagnose_if(p != NULL)` is rejected where `diagnose_if(p != 0)` is accepted,
  so a precondition has to spell the null pointer `0`
  (`clang-diagnose_if-null-constant.md`).
