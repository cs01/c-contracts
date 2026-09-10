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

Not yet ported from the reference implementation: the CBMC emitter behind
`c-contracts prove`.

The reference implementation is a clang fork that parses all of this as real
grammar. It is not shipped and not required; it exists as the differential
oracle this tool is checked against.

`docs/worksheets/2026-09-09-port-from-fork.md` is the working plan: what is
done, what the two remaining ports need, the gotchas already paid for, and the
open questions.
