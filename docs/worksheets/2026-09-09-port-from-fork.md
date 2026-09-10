# Worksheet: contracts for C without a clang fork

Started 2026-09-09, still live 2026-09-10. Status: **phase 1 complete and
published** at github.com/cs01/c-contracts. The tool skeleton, B2 and B3 are
done, all three open questions in section 7 are settled, and the whole thing
runs against real zstd.

Since then the annotation language itself changed shape: one spelling instead of
two, everything prefixed `contract_`, header version 2. Section 10 is what is
left.

A fresh agent should be able to pick this up from this file alone. Read it,
then `README.md` for what the thing is. Section 5 records what B3 turned out to
be, which is not quite what section 5 originally planned.

---

## 1. Goal

Give C contracts that a **released** clang checks. No fork of LLVM, no compiler
patch, no plugin, no `-fplugin=` threaded through anyone's build system.

There is a clang fork at `~/git/llvm-contracts` (branch `contracts-c-dev`) that
implements all of this as real grammar, in ~6.5k lines of in-tree changes. It is
**not the product**. It stays alive as the differential oracle: whatever this
tool emits must match what the fork emits. Do not rebase it on a schedule; it
only has to build.

Three checking levels, from the fork's README:

| level | what it catches | who does it here |
|---|---|---|
| 1. front end | is the contract well formed? | stock clang (preconditions) + the ghost pass (postconditions) |
| 2. call site | does any caller break it? | stock clang folds constants; **B2** catches it through a variable |
| 3. CBMC | is it true for every input? | **B3** |

## 2. The mechanism, and why it works

`c_contracts.h` picks one of four targets at include time. Two matter here.

**Preconditions become `diagnose_if`.** Its argument is parsed in the
function's own prototype scope (`Attr.td`'s `ParseArgsInFunctionScope` bit), so
a released clang gives a precondition name lookup, type checking, and folding at
every call site. Verified: stacks, inherits onto the definition, works in C89,
`-Wuser-defined-warnings` controls it. A *custom* warning group is rejected
("unknown warning group"), so don't try to invent `-Wcontract-violation`.

**Everything else becomes an `annotate` string**, which clang lexes but never
parses. `#P` deliberately does not expand the clause: a project macro inside it
means the right thing only in the TU it came from, which is exactly where the
ghost pass puts it back.

**The ghost pass** (`src/Ghost.cpp`) is what replaces the fork's parser. For
each postcondition it synthesizes

```c
static int __c_ghost_0(void *dst, size_t dstCap) {   /* param list verbatim */
  __typeof__(decode(dst, dstCap)) c_result;          /* result binding */
  return (c_result <= c_old(dstCap));                /* the clause */
}
```

appends it to the TU, reparses, and maps diagnostics back to the annotation in
the user's source. `__typeof__(f(args))` works in C89 and is how a C function
names its own return type, which it otherwise cannot. In a scope whose
parameters *are* the entry values, `old(x)` is `x`, so the preamble defines
`c_old(E)` to `(E)`.

## 3. What is done, and how to build it

### Fork: `~/git/llvm-contracts`, branch `contracts-c-dev`

The fork is **not the product**. It stays alive as the differential oracle and
as the source the ports are copied from. It only has to build.

It carries the shared `clang/lib/Headers/c_contracts.h`, which is the one file
both repos own. `tools/sync-header.sh` keeps the vendored copy honest; the fork
is the source of truth.

Gate: **37/37 lit.**

```sh
cd ~/git/llvm-contracts
ninja -C build-arm clang clang-resource-headers
./build-arm/bin/llvm-lit -q clang/test/{Sema,Parser,CodeGen,Driver}/c-contracts*.c \
  clang/test/PCH/c-contracts.c clang/test/Preprocessor/feature-c-contracts.c \
  clang/test/Frontend/c-contracts-cxx-rejected.cpp
```

`ninja clang-resource-headers` is **not optional** after touching the header:
lit reads the copy under `build-arm/lib/clang/24/include/`, and a stale copy
produces failures that have nothing to do with your change. Note that ninja
compares mtimes, so restoring a header from a backup with `cp`/`mv` can leave
the stale copy in place -- `touch` it before rebuilding.

Header tests worth knowing about, because they are what breaks when you edit it:

| test | pins |
|---|---|
| `c-contracts-macro-layer.c` | all four targets on one source, prefixed spelling |
| `c-contracts-macro-inplace-post.c` | `post` checked in place, and the `-DC_CONTRACTS_NO_INPLACE_POST` opt-out |
| `c-contracts-macro-strip.c` | the GCC/MSVC/tcc branch, reached with `-DC_CONTRACTS_STOCK=0` |

### This repo: `~/git/c-contracts`, branch `main`

| file | does |
|---|---|
| `CMakeLists.txt` | `find_package(Clang)`; `project(... C CXX)` because LLVMConfig probes libedit with `check_include_file` |
| `src/Contract.h` | `ClauseKind`, `Clause`, `Contract`, and the entry points |
| `src/Extract.cpp` | `DiagnoseIfAttr` -> Pre, `AnnotateAttr` `"contract_post:"`/`"contract_returns:"` -> the rest, and `headerVersion()` |
| `src/Ghost.cpp` | synthesis, reparse, diagnostic remapping. Now only needed for `returns` and the frame, since `post` is checked in place |
| `src/CallSite.{h,cpp}` | B2, the CFG dataflow |
| `src/main.cpp` | subcommand dispatch, the options, exit status, pass wiring |
| `src/CProver.{h,cpp}` | B3's port: the CBMC printer, the generated entry point, the writes-without-fresh check, the clause canonicaliser |
| `src/Prove.{h,cpp}` | B3's pipeline: preprocess, goto-cc, goto-instrument, the solver race, vacuity, caller mode |
| `test/run.sh` + 6 cases | fixture runner, `UPDATE=1` to rebless, filter arg |
| `test/prove.sh` + 7 cases | the CBMC tier end to end; skips when cbmc is absent |
| `test/differential.sh` | this lowering against the fork's, clause for clause; skips when the fork is absent |
| `test/zstd.sh` | both tiers on real zstd; skips when the checkout is absent |
| `test/header.sh` | the header alone, with **nothing but a C compiler** |

Gates: **12/12 header, 6/6 fixtures, 7/7 proofs, differential as recorded, zstd
2/2**, plus 35/35 lit in the fork.

`test/header.sh` is the one that matters for a vendored copy: it needs no LLVM,
no cbmc and no fork, because a project that copies the header and edits it --
everybody edits it -- has none of those. `C_CONTRACTS_VERSION` exists for the
same reason, and `RequiredHeaderVersion` in `src/Contract.h` moves with it, so
a stale vendored copy is reported rather than silently producing no clauses.

```sh
cd ~/git/c-contracts
cmake -G Ninja -B build -DCMAKE_PREFIX_PATH=$(brew --prefix llvm)
ninja -C build
./test/run.sh build/c-contracts && ./test/prove.sh build/c-contracts &&
  ./test/differential.sh build/c-contracts && ./test/zstd.sh build/c-contracts &&
  ./tools/sync-header.sh
```

All four are registered with ctest, and the three with prerequisites SKIP
loudly rather than failing: a suite that cries wolf for a missing checkout is a
suite nobody reads.

`UPDATE=1 ./test/run.sh build/c-contracts [filter]` reblesses. Read the diff
first: a fixture changing usually means a real behaviour change, not a stale
expectation. Two of the five (`extract`, `badpost`) were reblessed once, when
the markers started quoting clauses exactly as written; that was an improvement,
and it is the only time so far.

**The tool needs clang's own headers and cannot find them.** It does not live
inside an LLVM install, so it cannot derive the resource directory from argv[0]
the way the driver does. `CMakeLists.txt` bakes in `C_CONTRACTS_RESOURCE_DIR`
and `main.cpp` adds `-resource-dir=` to every parse. Without it a source that
includes `<stddef.h>` parses with `size_t` unknown and every clause naming it
collapses to a recovery expression -- silently, because the tool ignores
clang's own diagnostics by design.

Working end to end:

```
$ ./build/c-contracts t.c -- -std=c89 -Iinclude
t.c:8:36:  error: use of undeclared identifier 'dstCapp'
t.c:19:3:  warning: precondition n > 0 of 'allocate' is violated by this call
```

### Checking tiers as they actually stand

| clause | stock clang alone | this tool | CBMC |
|---|---|---|---|
| `pre` | folds at the call site | B2 catches it through a variable | `__CPROVER_requires` |
| `post` | **type-checked in place** | ghost (redundant, kept as fallback) | `__CPROVER_ensures` |
| `returns` | nothing | ghost type-checks it | `__CPROVER_ensures` |
| `assigns`, loop contracts | nothing | nothing | `__CPROVER_assigns`, checked by `prove --mode=enforce` |

## 4. DONE: B2 — the call-site dataflow pass

**Why it is needed even though `diagnose_if` exists.** Clang only fires when the
condition folds against the actual argument *expressions*. `int n = 0;
allocate(n);` folds nothing. The fork's pass is a CFG dataflow that tracks
variables and catches it. That is the whole of level 2's value over level 1.

**Landed 2026-09-09** as `src/CallSite.{h,cpp}`, wired into
`CheckConsumer::HandleTranslationUnit`, with `test/cases/callsite.c`. Suite 4 ->
5. The rest of this section is kept because it records why the pass is shaped
the way it is; the two notes below are what actually changed against the plan.

**What the plan got wrong.** `valueFromPost` could not be ported at all and
returns unknown: `Clause::Cond` is null for Post and Returns, so the
compositional case (`p = allocate(n); use(p);` learning non-null from
`allocate`'s postcondition) is unavailable out of tree. That is the real
precision cost of not having the fork.

**A second place the same invariant bites, which the plan did not anticipate.**
Keying the *substitution* on parameter index fixes `checkCall`, but entry
seeding writes through `refine` into `State`, which was keyed on raw
`VarDecl*`. A contract spelled on a prototype names the prototype's
`ParmVarDecl`s while the body names the definition's, so the function's own
preconditions silently seeded nothing in the ordinary header-plus-`.c` layout --
a missing report rather than a false one, which is why every test stayed green.
`trackingKey` canonicalises every `State` and `AddressTaken` key to
`(canonical decl, parameter index)`. The fixture's case 5 was a *negative* test
and so could not catch it; case 5b is the positive one that does.

**Source:** `~/git/llvm-contracts/clang/lib/Analysis/ContractChecking.cpp`, 854
lines, and its header
`clang/include/clang/Analysis/Analyses/ContractChecking.h`.

It is close to a drop-in: **only 5 sites touch fork AST types**, all of them
"iterate this function's clauses". Everything else is `Expr`/`CFG`/`APSInt`
work that a released clang has.

| line | fork code | replace with |
|---|---|---|
| 10 | `#include "clang/AST/ContractSpecifier.h"` | `#include "Contract.h"` |
| 653-657 | `Call->getDirectCallee()->getContractDecl()`, iterate for `CK_Post` | **drop for v1** — see below |
| 687-704 | same for `CK_Pre` in `checkCall` | `collectContract(callee)`, filter `Kind == Pre`, use `Clause::Cond` |
| 728-730 | the function's *own* preconditions seed the entry state | `collectContract(FD)`, filter Pre |

**`valueFromPost` cannot be ported as-is, and is dropped for v1.** The fork
reads a callee's *postconditions* to learn that a returned pointer is non-null
(`char *p = xmalloc(n); f(p);` discharges `pre(p != 0)`). Here it cannot:
`Extract.cpp` leaves `Clause::Cond` **null** for Post and Returns, because a
postcondition arrives as an `annotate` string that clang only lexed. The typed
`Expr` exists only inside the ghost pass's reparse, which is a different
`ASTContext`, so its pointers cannot be evaluated against this TU's state.
Returning `AbstractValue::unknown()` loses precision and invents no reports,
which is the safe direction. Recovering it later means pattern-matching
`Clause::Text` for `result != 0` / `result != NULL` / a bare `result` on a
pointer -- worth doing only if a real case wants it.

**The second invariant has a cleaner fix than storing the declaring decl.** Key
the substitution on the parameter's *index* (`ParmVarDecl::getFunctionScopeIndex()`)
rather than on `ParmVarDecl` identity. A prototype and its definition disagree
about which `ParmVarDecl` objects exist but never about what position a
parameter sits in, so indexing sidesteps the question of which redeclaration
spelled the contract. `Evaluator`'s `Subst` becomes keyed on `unsigned`, and its
`DeclRefExpr` case maps a `ParmVarDecl` whose `DeclContext` is in the callee's
redecl chain through `getFunctionScopeIndex()`.

`Clause::Cond` is already the contract, not the `diagnose_if` condition — the
`!` is stripped in `Extract.cpp`'s `contractFromViolation`. Do not re-negate it.

**Invariant that must survive the port** (the comment at fork line ~745 explains
it, keep the comment): a single CFG sweep is *wrong*, not merely imprecise.
Skipping back-edge predecessors leaves a loop header holding the pre-loop state,
which is stronger than the truth, and the pass invents reports for
`int n = 0; for (...) n = i + 1; f(n);`. Iterate to a fixpoint; merge keeps only
facts identical on every predecessor.

**Second invariant:** the predicate names the parameters of whichever
declaration spelled the contract, usually a prototype in a header, not the
callee this call resolved to. A prototype and its definition have distinct
`ParmVarDecl`s, so the substitution map must be keyed on the *declaring* one.
`Contract::Fn` is the canonical decl; check that this is the one whose
`ParmVarDecl`s the `Cond` expression actually references, and if not, store the
declaring `FunctionDecl` on `Clause` too.

**Reporter:** the fork's `ContractViolationReporter` has two callbacks,
`reportPreconditionViolated` and `reportPreconditionNotGuaranteed`. Keep both;
the second is the "an integer range does not imply the callee's bound" case and
is deliberately separate because it is noisier.

**Wiring:** it needs an `AnalysisDeclContext` with a CFG. Build one per
`FunctionDecl` with a body in `CheckConsumer::HandleTranslationUnit`
(`src/main.cpp`), using `AnalysisDeclContextManager`.

**Done looks like:** a test case where the violation is only visible through a
variable, reported at the call site, silent when the variable's value is
genuinely unknown. Add it as `test/cases/callsite.c` + `.expected`.

## 5. DONE: B3 — the CBMC tier and `prove`

**Landed 2026-09-09.** `c-contracts prove <fn> <file> -- <flags>` runs the whole
level-3 pipeline. What follows is what B3 turned out to be, which is not what
the plan above expected in three places.

### The plan's biggest wrong assumption: there was almost nothing to port

The plan listed eleven functions from the fork's 1446-line
`clang/lib/Sema/SemaContracts.cpp` as the emitter half to port. Most of them
have **no job out of tree**. Preprocessing the same source with
`-DC_CONTRACTS_CPROVER` *is* the lowering: the header's own CPROVER target turns
every clause into `__CPROVER_requires` / `__CPROVER_assigns` /
`__CPROVER_loop_invariant`, with no compiler in the pipeline that understands
contracts. Verified before writing a line of C++, and it is the single fact the
rest of B3 rests on.

| fork function | out of tree |
|---|---|
| `formatCProverClause`, `printCProverContracts`, `printCProverLoopContracts`, `forEachLoopContract`, `EmitCProverUnit` | **not needed** -- macro expansion does it |
| `CProverPrinter`, `printContractExpr` | ported (~90 lines), used **only** to print the generated entry point |
| `emitContractHarness` | ported (~150 lines): it needs parameter *types*, so it needs the AST |
| `findContradictoryPrecondition` | **replaced by something stronger**, see the vacuity note below |
| `findConflictingHarnessFresh` | not ported. Known gap |
| `recordDoWhileRewrite`, `findContinueTargetingLoop` | **cannot be ported.** See below |

`src/CProver.cpp` (395 lines) is the port. `src/Prove.cpp` (629) is new: it is
the pipeline, not an emitter.

### The one thing only a compiler can do

`goto-instrument` refuses a loop contract on a `do`/`while`. The fork's parser
rewrites `do C { B } while (E)` into `while (1) C { B if (!(E)) break; }`
silently, so an author annotates shipping code where it stands. Out of tree
there is no such pass: **the source has to be restructured by hand**, and that
was necessary to prove zstd's `ZSTD_wildcopy` (section 9). This is the only
capability gap left between the two implementations, and it is worth stating
plainly to anyone weighing the fork against the tool.

### What `prove` does

```
preprocess with -DC_CONTRACTS_CPROVER  ->  goto-cc  ->
goto-instrument --apply-loop-contracts  ->  --enforce-contract  ->  cbmc
```

- **The entry point is generated from the preconditions.** `fresh(L, N)`
  allocates, everything else is assumed, and a parameter no clause mentions is
  left uninitialised -- nondeterministic in CBMC, which is the honest default.
  `--bound n=N` caps a size the contract leaves open, and it is emitted *first*,
  because a cap applied after the allocation that reads it bounds nothing.
- **`proofs/<fn>.proof.c` wins over the generated one**, for a project whose
  allocation shape needs saying by hand. `--proof-dir` moves it.
- **`--mode=auto`** checks the frame where it can, and falls back to the entry
  point when goto-instrument refuses, *saying which loop has no contract*.
  `--mode=enforce` makes the refusal an error and lists the loops.
- **`--caller=f`** verifies `f` against the callee's contract. This is the only
  mode in which a precondition is an obligation rather than an assumption, and
  therefore the only one in which `disjoint` does anything. The decisive
  experiment from section 7(a) is now `test/prove/caller.c`: `caller_alias`
  FAILED, `caller_ok` SUCCESSFUL, and deleting the clause makes the two agree.
- **The writes-without-fresh diagnostic** fires before a solver runs, naming
  both the pointer and the size to put in the missing clause. It found its
  target on real code the first time: zstd's `ZSTD_execSequence`.

### Vacuity: the probe is better than the plan's design

The plan said to run each harness twice, the second time with `assert(0)`
appended, and budgeted for doubled solver time. **Don't append; probe.** The
implementation runs the allocations and assumptions and then asserts false
*instead of* calling the function:

- it is nearly free, because it never runs the function body, so the cost the
  plan budgeted for does not exist and the caching escape hatch is unnecessary;
- appending after the call would also require the call to *return*, so a
  function that cannot terminate under its own contract would be reported as
  vacuous -- a wrong answer to a question about the preconditions alone.

It subsumes the fork's syntactic `findContradictoryPrecondition`, which only
catches literal contradictions on one variable. Gate-audited: `--no-vacuity` on
`test/prove/vacuous.c` reports VERIFICATION SUCCESSFUL, which is exactly the
silent-forever failure the gate exists to stop.

### The solver: race, do not choose

The plan's first design picked a solver from the shape of the harness, since
this tool generates it and therefore knows whether the extents stay symbolic.
**That is wrong, and the first real run proved it:** cbmc 6.11 with `--z3`
aborts with an invariant violation on the loop-contract binaries here. A tool
that picked one solver would report a crash where the other has a proof. So
`prove` races them, on solve.sh's rules: rc 10 is definitive, rc 0 only if no
property is UNKNOWN, anything else keeps waiting.

### A bug the fixed capture files hid, worth not repeating

Every step originally wrote to one `stdout.tmp`. When the z3 run crashed and
wrote nothing, the tool read the *previous* step's file and reported its
verdict. A crash that looks like an answer is the worst thing this tool could
do. Each step now has its own capture files, removed before the run.

### CBMC checks every assertion in the binary, not only the reachable ones

The vacuity probe cannot live in the same translation unit as the proof: with
`--function f`, CBMC still reports the `assert(0)` in an unreachable
`__contract_vacuity_f`, and every proof built from that binary FAILS. The two
are compiled apart. This cost an hour of looking for a bug in the frame check
that was not there.

### The differential gate, and what it found

`test/differential.sh` lowers each case both ways -- macro expansion here,
`-fcontract-emit-cprover-unit` in the fork -- and compares the clauses through
one canonicaliser (`c-contracts clauses`, whitespace and parentheses removed).
**Byte-for-byte, as the plan hoped, is not true.** The differences are recorded
in `test/differential.expected` rather than normalised away, so a *new* drift
and a recorded one disappearing both fail:

1. `c_range(p, 0, n)` emits `object_upto((p) + (0), ((n) - (0)) * sizeof(*(p)))`
   where the fork simplifies to `object_upto(p, n)`; `c_forall` likewise emits a
   redundant `i >= 0`. A macro cannot test whether its argument is literally
   zero. Cost, not meaning, and goto-cc folds it.
2. The fork **rejects** `returns (c_result > n)` for a by-value parameter and
   demands `old(n)`. Measured here: CBMC reads such a parameter in an `ensures`
   at its *entry* value, so the two spellings verify identically and the fork is
   stricter rather than righter. `test/prove/postentry.c` pins the behaviour.

**Gate-audited, and it failed the first audit.** Swapping `w_ok` for `r_ok`
inside `c_writable` passed cleanly, because no proof fixture used the predicate
layer -- the roles reach CBMC through `c_writes`, not through `c_writable`.
`test/differential/vocabulary.c` puts every spelling on one declaration; the
gate now fires on that swap and on two more.

### Still not done

- `findConflictingHarnessFresh` (two `fresh` clauses on one buffer with
  different sizes) is not ported.
- The fork's *checking* half is still unported, and the differential gate has
  now identified the first thing worth taking from it: the `post` rule that a
  by-value parameter must be named through `old()`.
- **A loop's `assigns` silently widens the function's frame.** Narrowing only
  the function's clause on `test/prove/enforce.c` still verifies, because CBMC
  grants the loop's own targets inside the loop and never checks that they lie
  within the function's. `test/prove/badframe.c` narrows both and fails, which
  is what pins the frame gate; the containment check is nobody's yet.

## 6. Gotchas already paid for

- **`ninja clang-resource-headers`** after touching the header, or lit reads a
  stale copy. Cost me one confusing failure.
- **`assigns` must be the last function-like macro in its chain.**
  `assigns(locations(a, b))` expands `locations` during the argument prescan and
  hands the next macro three arguments where it declared one. Forwarding
  `assigns(L)` to `c_assigns(L)` breaks `c-contracts-macro-unprefixed.c`. Each
  target spells `assigns` out. The comment in the header says so; keep it.
- **`CompilerInstance::ExecuteAction` prints its own "N errors generated" to
  stderr**, gated on `ShowCarets`, reading counts off your own
  `DiagnosticConsumer`. `setDiagnosticConsumer` does not suppress it. Pass
  `-fno-caret-diagnostics` to any sub-invocation.
- **Attribute order is not declaration order.** Clang groups the annotate
  markers apart from the `diagnose_if`s, so a `writes` clause and the `returns`
  below it come back inverted. `Extract.cpp` sorts by source location.
- **Print the clause from the `diagnose_if` message, not from the AST.**
  `printPretty` gives `__c_writable(((p)), ((n)))` where the source says
  `c_writable(p, n)`.
- **`-pedantic` reports every `diagnose_if` through `-Wgcc-compat`** — three per
  annotated declaration. The header suppresses it with a `#pragma clang
  diagnostic ignored`, inside the clang-only branch.
- **A custom `diagnose_if` warning group is an error** ("unknown warning
  group"). Use the default `-Wuser-defined-warnings`.
- **Ghost diagnostics must key on the ghost's *first* line**, not the predicate
  line: an error can land on the signature or the result binding above it, and
  probing upward from the predicate line finds the *previous* ghost.
- **`llvm::outs()` and `llvm::errs()` do not flush in a fixed order.**
  `test/run.sh` captures them separately and joins them deterministically.

### Paid for on 2026-09-09

- **The argument prescan bites anything that stringizes, not just `assigns`.**
  `#P` only sees raw text when the stringizing macro is the *first* one the
  author's text reaches. `post(P)` forwarding to `c_post(P)` expanded the
  predicates on the way through, so `old(n)` reached the marker as `(n)`. `post`
  and `returns` are now spelled out per target, like `assigns` always was. If
  you add another clause that quotes itself, spell it out per target from the
  start.
- **ninja compares mtimes, so restoring a file from a backup can leave a stale
  resource-header copy.** `touch` the header before rebuilding. This cost a
  confusing "the test fails after I reverted my change".
- **A negative test cannot pin a positive behaviour.** B2's fixture had a case
  for "the function's own preconditions seed the entry state" that asserted
  *silence*. It passed whether or not seeding worked -- and seeding was in fact
  broken for the ordinary prototype-plus-definition layout. Any feature whose
  success looks like "no output" needs a case where the feature's absence
  *produces* output.
- **`pre (p != NULL)` does not compile.** `NULL` is `((void *)0)`, and a cast to
  a pointer is not a constant expression in C's evaluator, so `diagnose_if`
  rejects it: "attribute expression never produces a constant expression".
  `pre (p != 0)` is fine, `pre (!p)` is fine, and C++ accepts all of them. This
  is the first thing a real project hits -- zstd's `ZSTD_execSequence` hit it on
  the first build. Nothing in the header can work around it, because the clause
  reaches `diagnose_if` exactly as the author wrote it. Written up ready to file
  in `docs/clang-diagnose_if-null-constant.md`.
- **`writes_nothing`, `c_result`, `c_ssize_t` and `c_ghost` have no unprefixed
  spelling**, deliberately: they are object-like and would rewrite every bare
  occurrence of a common word. `void f(int n) pre (n > 0) writes_nothing {}`
  fails with "expected function body after function declarator", which points at
  the macro and says nothing about why. Write `c_writes_nothing`.
- **The tool swallows clang's own diagnostics**, so a source that does not parse
  reports as a source with no contracts. `prove` now says so explicitly when the
  translation unit had errors; anything else added here should too.
- **`llvm::ReversePostOrderTraversal<CFG *>` dereferences null.** `if (0)`,
  `while (0)` and `if (1) ... else ...` leave the pruned edge in the CFG with no
  block behind it, and `po_iterator` walks into it looking for grandchildren.
  This segfaulted the call-site pass on the first zstd translation unit it was
  pointed at. `CallSite.cpp` now computes the order itself, skipping nulls;
  `test/cases/prunedbranch.c` pins it *positively* -- the pass must still reach
  and report the call after the pruned branches.
- **CBMC checks every assertion in the goto binary**, not only those its
  `--function` entry point can reach. Anything that deliberately asserts false
  needs its own translation unit.
- **Write the gate, then break it on purpose and watch it fail.** Every gate
  added this session was audited that way, and two of them did not fail on the
  first attempt: a loop fixture with a literal bound made the broken pass drop
  the block silently instead of misjudging it, and a branch-merge fixture's
  outcome depended on which predecessor the CFG happened to list first. Both
  needed rewriting before they were tests at all.

## 7. Decisions and open questions

**(a) `is_fresh` vs `w_ok` — DECIDED 2026-09-09: grow a separation concept.**

`writes(p, n)` keeps meaning exactly what it says: this memory is valid to
write. It lowers to `__CPROVER_w_ok` and it does **not** imply anything about
aliasing. The aliasing claim gets its own clause, which a reader can see in the
annotation. The rejected alternative was defining `writes`+`reads` on one call
to imply disjointness: that makes the annotation assert something nobody can
find in the words, which is the failure this whole project argues against.

The evidence that forced the question, measured on the simplest function that
exists:

```c
void zero(unsigned char *p, size_t n) writes (p, n) { ... }
```

| clause | lowering | CBMC |
|---|---|---|
| `writes (p, n)` | `w_ok` | 10 of 41 failed — **VERIFICATION FAILED** |
| `pre(fresh(p,n)) assigns(range(p,0,n))` | `is_fresh` | 0 of 37 failed — **SUCCESSFUL** |

Differential-checked against the pre-change header: identical, so this is
pre-existing and not a regression from the stock-clang target.

What the decision settles:

- **Single-buffer separation needs no new surface.** `fresh(p, n)` is already
  spelled in all four targets (`c_fresh`, header lines 124 / 174 / 270) and
  already lowers to `__CPROVER_is_fresh`. It is the concept; it just was not
  documented as the thing that makes `writes` discharge.
- **Two-buffer disjointness has no spelling yet.** Add `disjoint(a, b)` ->
  `c_disjoint(A, B)`, lowering to `!__CPROVER_same_object((A), (B))` under
  `C_CONTRACTS_CPROVER` and following `c_same_object`'s existing per-target
  treatment everywhere else (a never-defined `__c_disjoint` on the stock-clang
  and fork targets, nothing on the strip target). `c_same_object` is the model
  to copy, line for line.
- **The README's `writes` examples are wrong for anyone who runs `prove`.**
  Every example meant to be provable needs `pre(fresh(p, n))` next to its
  `writes(p, n)`. Fixing them is part of this change, not a follow-up.
- **`prove` should say this rather than let it surface as 10-of-41 failures.**
  A function with `writes`/`writes_n` on a pointer parameter and no `fresh` on
  that same pointer gets a diagnostic naming the clause it is missing. B3 scope.

Cost accepted: a provable buffer-writing function spells two clauses where the
README used to show one.

**Implemented and measured 2026-09-09.** `c_disjoint` / `disjoint` is in the
header across the three targets that have a predicate layer, defined as
`!same_object` so it needs no new never-defined helper on the stock-clang
target. Lit was 35/35 as of that commit (37 now, after the two tests added
later the same day), and the new CHECK was perturbed to confirm it fails when it
should. Measured with the real toolchain (`proofs/verify-contract.sh`, cbmc
6.x):

| function | clauses | CBMC |
|---|---|---|
| `zero_writes` | `writes (p, n)` | 10 of 139 failed — FAILED |
| `zero_fresh` | `pre (fresh(p,n))` + `assigns (range(p,0,n))` | 0 of 139 — SUCCESSFUL |
| `copy` | both `fresh` + `disjoint` | 0 of 139 — SUCCESSFUL |

That reproduces the original finding on the new header, so no regression.

**What the measurement taught, and it changes how `disjoint` must be
documented and tested.** Two dead ends worth not repeating:

1. `disjoint` cannot be demonstrated on a function whose buffers are both
   `fresh`: `fresh` already means "distinct from every other object in the
   proof", so the clause is redundant there and removing it changes nothing.
2. It also cannot be demonstrated with `writable`/`readable` instead of
   `fresh`. Both spellings of a `stamp` function that claims `post (src[0] ==
   old(src[0]))` fail *identically*, 12 of 90, and the postcondition is not
   among the failures — the `w_ok`-without-`fresh` noise (bad harness storage)
   swamps it before the aliasing property is ever reached.

The clause bites on the **caller** side, which is where a precondition is an
obligation rather than an assumption. Enforcing `copy` assumes `disjoint`;
verifying a caller against `copy`'s contract proves it. The decisive experiment,
worth keeping as a fixture when `prove` grows a caller mode:

```sh
goto-instrument --replace-call-with-contract copy cs.goto out.goto
cbmc --function caller_alias out.goto
```

with `caller_ok` passing all four preconditions, `caller_alias` (`copy(a, a, 4)`)
failing precondition **.4**, the `disjoint` one, and that same caller verifying
SUCCESSFUL once the clause is deleted.

**(b) Vacuity gate — DECIDED 2026-09-09: on by default, `--no-vacuity` to opt
out.** Rationale and mechanism are in section 5; this settles the default.

**(c) Postconditions on the declaration — DONE 2026-09-09, for `post`.**

`post` is now checked in place by stock clang, no tool and no upstream patch
required. A `post` is a result-independent fact, so it names nothing that is not
already in scope where `diagnose_if` parses its argument. The header emits, next
to the marker:

```c
__attribute__((diagnose_if(0 && (P), "postcondition " #P, "warning")))
```

`0 &&` folds the condition to false so it can never fire at a call site, and
clang type-checks the operand regardless. `c_old(E)` is defined to `(E)` in that
target, on the same reasoning the ghost preamble uses: in a scope whose
parameters are the entry values, `old(x)` is `x`.

`returns` cannot join it, and the reason is a **clang crash, not a design
choice**: binding a name to the function's own return type needs a declaration,
so it needs a statement expression, and a statement expression inside a
late-parsed attribute argument segfaults the parser. Reproduced on Homebrew
22.1.8 and on this tree's trunk build:

```c
void f(int n) __attribute__((diagnose_if(0 && ({ int r; r > n; }), "m", "warning")));
```

Not yet reported upstream. The full write-up, with the stack, the affected
versions and the `enable_if` contrast that localises it to the `diagnose_if`
path, is in `docs/clang-diagnose_if-stmtexpr-crash.md` -- ready to file. Until
it is fixed, `returns` stays a quoted marker and the ghost pass is what checks
it, so **the ghost pass does not go away**.

`__typeof__(f(args))` *does* resolve inside `f`'s own `diagnose_if` — the
function is in scope there — so the return type is nameable in place. Only
binding a name to a value of it is blocked. If the crash is fixed, this becomes
a ~5-line header change and the ghost pass becomes a fallback.

**The trap this hit, worth not re-hitting.** Defining `c_old` broke the marker
text: `post(P)` forwarded to `c_post(P)`, and passing `P` to another macro
triggers the argument prescan, so `old(n)` reached the quoted marker as `(n)`.
This is the `assigns` gotcha from section 6 in a second place. The fix is the
same one: `post` and `returns` are now spelled out per target in the
`C_CONTRACTS_NO_PREFIX` block rather than forwarding, so `#P` is the first macro
the author's text reaches. That *improved* fidelity beyond the starting point --
the markers now quote the clause exactly as written, where before they showed
one prescan level of expansion (`c_old(dstCap)` for a source that said
`old(dstCap)`). `test/cases/extract.expected` and `badpost.expected` were
reblessed for that, and it is the only thing that changed in them.

Escape hatch: `-DC_CONTRACTS_NO_INPLACE_POST`. The one shape the in-place check
rejects that the tool accepts is a `post` naming a file-scope declaration that
appears *later* in the translation unit.

Gates: fork lit 36/36 (new `c-contracts-macro-inplace-post.c`, gate-audited by
removing the feature and confirming it fails), tool 4/4 (new
`test/cases/inplacepost.c`, which also pins that a bad `post` and a bad `returns`
are each reported exactly once, from their two different checkers).


## 8. Not verified locally

**Real GCC.** Still no gcc on this machine, and no tcc; `/usr/bin/gcc` is Apple
clang. Someone with a real gcc should still run `gcc -std=c89 -pedantic -Wall
-Wextra` over `test/cases/*.c` and confirm silence, and the same for MSVC.

What *is* now tested, 2026-09-09, is the branch those compilers land on. It had
no coverage at all, which is the worse half of the problem: clang always has
`diagnose_if`, so the strip branch was unreachable from lit and nothing checked
the header's headline promise. Autodetection now only fires when
`C_CONTRACTS_STOCK` is not already set, so `-DC_CONTRACTS_STOCK=0` reaches that
branch from a clang (and doubles as the switch for a build that wants the
annotations present but inert). `clang/test/Sema/c-contracts-macro-strip.c` pins
that every clause preprocesses away to the bare declaration -- no attribute, no
annotate string, no leftover predicate call needing a definition at link time --
that a loop header is untouched, and that the result is clean under `-std=c89
-pedantic -Wall -Wextra`. Gate-audited by removing the guard and confirming the
test fails.

That leaves only *GCC's own preprocessor and parser* unverified, rather than the
header's behaviour on that branch.

**No `bitwuzla` or `cvc5` on this machine.** The solver race runs `sat` and `z3`
here; the other two branches are code nobody has executed.

## 9. Against real zstd

Everything above is fixtures. This section is the tool pointed at a codebase
that was not written for it: `~/git/zstd`, branch `contracts-annotations`
(`cs01/zstd`), whose decoder carries the portable annotations.

`test/zstd.sh` is the gate, and it SKIPs when the checkout is absent.

**What had to be fixed before it worked at all**, in order:

1. zstd's vendored `lib/common/c_contracts.h` was 268 lines and predated the
   whole stock-clang target. Re-vendored from `include/c_contracts.h`.
2. `pre (op != NULL)` did not compile. See section 6.
3. The call-site pass segfaulted on the first translation unit. See section 6.
4. `goto-cc` cannot parse Homebrew clang's `arm_vector_types.h`, which zstd
   reaches through `compiler.h` -> `arm_neon.h`. The project passes
   `-U__ARM_NEON -DZSTD_NO_INTRINSICS`, exactly as the fork's harnesses do.

**What it then found.** The first `prove` run of `ZSTD_wildcopy` reported
undefined behaviour, independently rediscovering the fork's
`FINDING-wildcopy-pointer-subtract.md`:

```c
ptrdiff_t diff = (BYTE*)dst - (const BYTE*)src;   /* before the branch */
```

Subtracting two pointers is defined only within one object, and that is exactly
the overlap case -- so this is undefined for every no-overlap caller, which is
all of the hot ones. Moved into the `&&` that already tests for overlap, where
it short-circuits.

**The result.** With the fix, and with loop contracts added to wildcopy's two
loops in the portable spelling (both `do` loops rewritten to `while (1)` by
hand, since nothing out of tree can do it for us):

```
  1 reads a real translation unit              PASS   10 clauses read out of a stock parse
  2 ZSTD_wildcopy is memory safe, unbounded    PASS   2s, budget 60s
```

Unbounded: the buffers are symbolically sized and the loop contracts discharge
the loops, so there is no `--unwind` and no cap on `length`. Two seconds against
the fork's 60s CI budget from e2e case 7.

**Gate-audited, four ways.** Allocating two bytes less than
`length + WILDCOPY_OVERLENGTH` FAILS; allocating `length` FAILS; removing the
loop contracts and relying on `--unwind` FAILS its unwinding assertion once the
length cap is lifted; and one byte short still passes -- `WILDCOPY_OVERLENGTH`
has exactly one byte of margin over what wildcopy touches, which is a fact about
zstd rather than about the gate.

**zstd itself is unharmed.** `libzstd.a` and the CLI build, round-trips at
levels 1/3/9/19 match, and `tests/fuzzer` completed 8540 tests clean.

**Where the proof's assumptions live.** `test/zstd/proofs/ZSTD_wildcopy.proof.c`
is hand written on purpose. The separation it assumes belongs to the *proof*,
not to the function: wildcopy's real callers hand it interior pointers into one
output buffer, so `fresh(dst, ...)` on the function itself would be false and
would oblige every caller to something zstd does not do. Keeping it in the proof
file leaves zstd's contract saying only what callers actually owe.

**Merged with the other annotation branch, 2026-09-10.** Someone else's session
had pushed to `cs01/zstd contracts-annotations` independently: the same
pointer-subtraction fix, equivalent loop contracts, and a `CONTRACTS.md`.
Resolved to this side's source, which was ahead in three ways -- the
`contract_` spelling, the re-vendored header, and the `do` loops rewritten to
`while (1)` so `goto-instrument` will accept a contract on them. Kept their
`CONTRACTS.md` and their `ZSTD_safecopy` contract, which ports cleanly and took
zstd from 10 clauses to 12. Dropped their `ZSTD_execSequence` preconditions:
they rest on `pointer_in_range`, which is in neither header, and section 10
records why that predicate does not currently work anyway. Their branch compiled
only because its vendored header predated the stock-clang target and stripped
every clause.

**`ZSTD_execSequence` does not converge.** 180s with both solvers, no verdict.
It is the hardest function in the set -- COST.md says so, and the fork needed
`--object-bits 12` and a much more careful harness. The generated entry point
also cannot allocate for it, because the contract has `readable(*litPtr, ...)`
with no `fresh`, which is precisely what the writes-without-fresh diagnostic
says when you run it.

## 10. 2026-09-10: one spelling, and what is still open

### The language surface changed

`c_pre` and the `C_CONTRACTS_NO_PREFIX` opt-in are both gone. There is one
spelling, `contract_pre`, always available. Three reasons, in order of weight:

1. The bare spelling took words as common as `pre`, `range` and `result` out of
   a project's namespace, and making it opt-in only moved that decision to
   whoever included the header first.
2. `c_` is a namespace other people are already in: zstd's own source has a
   `c_str`. And `c_result` / `c_ssize_t` / `c_ghost` / `c_writes_nothing` are
   object-like, so they rewrote every occurrence of a fairly generic token.
3. It deletes a whole bug class. `post`, `returns` and `assigns` had to be
   spelled out per target because forwarding `pre(P)` to `c_pre(P)` triggered
   the argument prescan and expanded the predicates before `#P` could quote
   them (section 6). Nothing forwards to anything now, so a marker quotes
   exactly what the author wrote.

Lowercase rather than AWS's `CONTRACT_REQUIRES`, because a clause should read as
part of the declaration rather than as macro noise.

Cost: lit lost two tests that existed only to check the opt-in, and everything
downstream had to move. Version 2.

### What the README got wrong, and how it was caught

Worth recording because the same trap will recur: **every example in a README is
a claim, and none of them were being run.** Extracting the headline block and
executing it found that it did not compile (`size_t` with no include), and then
that it did not produce the output printed under it -- an unannotated `for` loop
drops `prove` into harness mode with no bound, where it does not terminate. The
roles table also showed `((char *)p)[0 : n]`, which is the fork's grammar: the
header drops it silently and goto-cc then rejects it.

There is no gate on the README. That is the obvious next one.

### `__CPROVER_pointer_in_range` is unsatisfiable under `--enforce-contract`

Found while merging the other zstd branch, which used a `pointer_in_range`
predicate that is in neither header. Measured on cbmc 6.11:

```c
void f(char *base, char *p)
  __CPROVER_requires(__CPROVER_is_fresh(base, 64))
  __CPROVER_requires(__CPROVER_pointer_in_range(base, p, base + 64))
{ __CPROVER_assert(0, "vacuity probe"); }
```

`goto-instrument --enforce-contract` then `cbmc` reports **VERIFICATION
SUCCESSFUL**, so the preconditions cannot be satisfied and anything proved under
them is proved of nothing. `__CPROVER_pointer_in_range_dfcc` is the variant
CBMC's contracts machinery uses; `goto-instrument --dfcc` segfaulted on the test
harness here, so whether the predicate is usable at all in this mode is open.

**The lesson is about method, not about that predicate.** The first reading of
this experiment was that the predicate *was* modelled, because an assertion
after it verified clean. That is what a vacuous assumption looks like from the
inside. Always run the `assert(0)` probe before believing a SUCCESSFUL.

### Open, in the order worth doing

1. ~~`contract_writes_nothing` is object-like.~~ **Done 2026-09-10**, header
   version 3: it takes empty parens like every other clause.
2. **`prove` has no project mode.** One function per invocation, no caching, no
   report. The empirical study on unit proofs (arXiv 2503.13762, 73 proofs over
   four embedded OSes) measures 87 minutes to write a proof and 61 minutes to
   run one; without caching a suite is unusable. This is the thing standing
   between the tool and a user.
3. **A gate on the README's examples.** See above.
4. **Three unit conventions.** `contract_writes(p, n)` is bytes,
   `contract_writes_n(p, n)` is elements, `contract_range(p, lo, hi)` is
   elements and half open. Each has a reason; together they are a trap. No fix
   proposed, but it should be a deliberate decision rather than an accident.
5. **`findConflictingHarnessFresh`** is still unported from the fork.
6. **The `post` rule the differential gate found:** the fork demands `old(n)`
   for a by-value parameter where this accepts a bare `n`. Measured equivalent
   under CBMC, so it is strictness rather than soundness, but it is the first
   thing worth taking from the fork's checking half.
7. **A loop `assigns` silently widens the function frame** (section 5). Nobody
   checks containment.

### The reference section was one table too few

Splitting it was not cosmetic. Writing down "a predicate is true or false and
goes inside a clause" made it obvious that half the predicate table were not
predicates: `contract_result` is a value, `contract_ssize_t` a type, and
`contract_range` / `contract_locations` are frame locations that are only valid
inside `contract_assigns`. They are now three tables, and the taxonomy at the
top of the reference names all five kinds. Defining a category is a good way to
find the things filed under it wrongly.

### Roles are used zero times on the only real codebase

Measured 2026-09-10 over the annotated zstd, excluding the vendored header:

```
contract_pointer_offset  32     contract_reads      0
contract_invariant       17     contract_writes     0
contract_pre             12     contract_reads_n    0
contract_same_object      8     contract_writes_n   0
contract_readable         3
```

Roles cost four names, a byte-versus-element trap and a reference table, and
earn nothing so far. The reasons real annotations skip them are visible in the
source: a size a role cannot take (`length + WILDCOPY_OVERLENGTH`), a `p != 0`
the author did not want bundled in, or a frame stated differently.

They are **not novel** either. A role is macro sugar over clauses; the only
judgment in them is which clauses to bundle, and decision (a) in section 7 is
the one call that mattered -- that `contract_writes` claims nothing about
aliasing.

Not deleted yet, because n=1 and the one codebase was annotated by the same
people who wrote the header. The README now says plainly that they are sugar
and that nobody has used them. If that is still true after a second project,
delete them: this language's whole argument is that it is small enough to
vendor.

### Distribution, which is the actual product question

The header is the artifact people will copy. `stb`-style vendoring is the right
mechanism and needs nothing built. What it still lacks: a CMake `INTERFACE`
target for `FetchContent`, and a vcpkg or Conan port. Both trivial for a
header-only file.

The **binary** is the harder half and is worse than the incumbents'.
`pip install cbmc-starter-kit` against "have an LLVM install shipping
ClangConfig.cmake, then cmake and ninja this". A Homebrew formula and prebuilt
per-platform binaries would fix it; neither exists.
