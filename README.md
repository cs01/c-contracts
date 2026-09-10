# c-contracts

Write what a C function requires. Your compiler checks it.

```c
#define C_CONTRACTS_NO_PREFIX
#include "c_contracts.h"

int *allocate(size_t n) pre (n > 0);
```

```
$ clang -c demo.c
demo.c:14:22: warning: precondition n > 0 is violated by this call
   14 |   int *p = allocate(0);
      |                      ^
```

That is the whole setup. One header, copied into your tree. No tool, no build
step, no plugin, no fork. It works on any clang from the last decade, and the
same file still builds under GCC, MSVC and tcc, where every clause disappears.

```sh
curl -O https://raw.githubusercontent.com/cs01/c-contracts/main/include/c_contracts.h
```

## What you get for free

| you write | you get |
|---|---|
| `pre (n > 0)` | a warning at every call site that breaks it |
| `post (cap > 0)` | the expression type-checked where you wrote it |
| `writes (dst, n)` | both of the above, for the buffer |

Free means free: no tool installed, no flag to pass, no separate build. Rename a
field and the contract that named it fails to compile, like the rest of your
code.

## Optional: the tool

Two things a compiler alone cannot do.

```
$ c-contracts check demo.c -- -Iinclude
demo.c:11:3: error: use of undeclared identifier 'dstCapp'
demo.c:19:3: warning: precondition n > 0 of 'allocate' is violated by this call
```

The first is `returns (...)`, which needs the return type bound to a name. The
second is a violation that travels through a variable, which constant folding
cannot see:

```c
int n = 0;
allocate(n);        /* clang folds nothing here; the dataflow pass catches it */
```

## Optional: proofs

```
$ c-contracts prove zero demo.c -- -Iinclude
mode: enforce (frame checked)
vacuity: zero's preconditions are satisfiable
zero: VERIFICATION SUCCESSFUL
```

Needs CBMC. Proves the contract for *every* input, not for the cases you thought
of. The entry point is generated from the preconditions, so there is no harness
full of hand-written assumptions that nobody reviews.

On zstd's decoder this proves `ZSTD_wildcopy` memory-safe for every length in two
seconds, and found undefined behaviour on the first run.

## Reference

### Clauses

After the parameter list, before the `;` or the `{`. They stack.

| clause | means | checked by |
|---|---|---|
| `pre (P)` | caller must establish `P` | compiler, tool, CBMC |
| `post (P)` | `P` holds on return | compiler |
| `returns (P)` | `P` holds on return, may name `c_result` | tool, CBMC |
| `assigns (L)` | nothing outside `L` changes | CBMC |
| `c_writes_nothing` | the frame is empty | CBMC |

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
| `c_result` | the return value; `returns` only |
| `range (p, lo, hi)` | elements `[lo, hi)` of `p`, for a frame |
| `locations (a, b)` | two frame locations; nests for more |
| `c_forall (i, lo, hi, P)` | `P` for every `i` in `[lo, hi)` |
| `c_ssize_t` | signed, wide enough for a pointer offset |

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

`c_ghost` marks a variable that exists only for an annotation, so it does not
become `-Wunused-variable` where the clauses vanish:

```c
BYTE* const opStart c_ghost = op;
```

## Things that will bite you

- **Write `0`, not `NULL`.** `pre (p != NULL)` does not compile. `NULL` is
  `((void *)0)` and a cast to a pointer is not a constant expression in C, so
  the attribute is rejected. `pre (p != 0)` and `pre (!p)` are fine.
- **Every name has a `c_` form** (`c_pre`, `c_fresh`, `c_range`) that always
  works. The short spellings need `#define C_CONTRACTS_NO_PREFIX` first, because
  they take common words.
- **Only function-like names get short spellings.** `pre(` expands, `int pre;`
  does not. So `c_result`, `c_ssize_t`, `c_ghost` and `c_writes_nothing` have no
  short form.
- **No short `forall`.** Write `c_forall (i, lo, hi, P)`.
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

## Why not just use CBMC

CBMC already has `__CPROVER_requires`. Use it directly if all you want is a
proof. Two differences:

- **`__CPROVER_requires` in your source breaks every build that is not CBMC.**
  Projects work around it with a private macro layer: AWS s2n has one, aws-c-common
  has a different one, and CBMC's docs note that repositories "may use their own
  names for some of them". This is that layer, as one vendorable file instead of
  a per-project copy-paste.
- **Their fallback is empty; this one is `diagnose_if`.** An `#else`-to-nothing
  contract is unparsed text until someone runs CBMC, so it silently rots. Here
  it is checked by every ordinary compile.

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

The header is the product and it is done. `check` is in. `prove` works and is
the least finished part: no project-level runs, no caching, no proof reports,
and a generated entry point that handles the easy cases.

`docs/` has the working record and two clang defects this ran into, written up
ready to file.
