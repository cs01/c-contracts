# Worksheet: contracts for C without a clang fork

Started 2026-09-09. Status: phase 1, the tool skeleton and **B2 are done and
committed**. All three open questions in section 7 are settled. B3 is the only
port left.

A fresh agent should be able to finish from this file alone. Read it, then read
`README.md` for what the thing is, then start at "Next: B3" in section 5.

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
| `c-contracts-macro-unprefixed.c` | the `C_CONTRACTS_NO_PREFIX` aliases |
| `c-contracts-macro-inplace-post.c` | `post` checked in place, and the `-DC_CONTRACTS_NO_INPLACE_POST` opt-out |
| `c-contracts-macro-strip.c` | the GCC/MSVC/tcc branch, reached with `-DC_CONTRACTS_STOCK=0` |

### This repo: `~/git/c-contracts`, branch `main`

| file | does |
|---|---|
| `CMakeLists.txt` | `find_package(Clang)`; `project(... C CXX)` because LLVMConfig probes libedit with `check_include_file` |
| `src/Contract.h` | `ClauseKind`, `Clause`, `Contract`, and the entry points |
| `src/Extract.cpp` | `DiagnoseIfAttr` -> Pre, `AnnotateAttr` `"c_post:"`/`"c_returns:"` -> the rest |
| `src/Ghost.cpp` | synthesis, reparse, diagnostic remapping. Now only needed for `returns` and the frame, since `post` is checked in place |
| `src/CallSite.{h,cpp}` | B2, the CFG dataflow |
| `src/main.cpp` | `--list`, `--warnings-as-errors`, exit status, and the pass wiring |
| `test/run.sh` + 5 cases | fixture runner, `UPDATE=1` to rebless, filter arg |

Gate: **5/5 fixtures.**

```sh
cd ~/git/c-contracts
cmake -G Ninja -B build -DCMAKE_PREFIX_PATH=$(brew --prefix llvm)
ninja -C build && ./test/run.sh build/c-contracts && ./tools/sync-header.sh
```

`UPDATE=1 ./test/run.sh build/c-contracts [filter]` reblesses. Read the diff
first: a fixture changing usually means a real behaviour change, not a stale
expectation. Two of the five (`extract`, `badpost`) were reblessed once, when
the markers started quoting clauses exactly as written; that was an improvement,
and it is the only time so far.

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
| `assigns`, loop contracts | nothing | nothing | the whole point of B3 |

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

## 5. Next: B3 — the CBMC emitter and `prove`

**Everything B3 depends on is settled.** All three questions in section 7 are
answered, the toolchain is present and was exercised end to end this session,
and the numbers below were measured rather than assumed. Start here.

### The environment, verified 2026-09-09

`cbmc`, `goto-cc` and `goto-instrument` are installed (`/opt/homebrew/bin`,
cbmc 6.x, which is what the loop-contract handling needs). No `z3` or
`bitwuzla`: `proofs/solve.sh` races whatever is installed, and without an SMT
solver the symbolically-allocated harnesses take minutes rather than seconds.

**`proofs/verify-contract.sh` defaults `CLANG` to `../build/bin/clang`, which
does not exist on this machine.** Pass `CLANG=$PWD/build-arm/bin/clang`. Every
invocation below does.

Also note `cc` is shadowed in this shell by something that prints `Copied:`.
Use `/usr/bin/clang -E` explicitly when driving the preprocessor by hand;
`verify-contract.sh` calls `cc` internally and works fine non-interactively.

### The numbers B3 has to reproduce

Measured with the real toolchain, on the simplest functions that exist:

| function | clauses | CBMC |
|---|---|---|
| `zero_writes` | `writes (p, n)` | 10 of 139 failed -- FAILED |
| `zero_fresh` | `pre (fresh(p,n))` + `assigns (range(p,0,n))` | 0 of 139 -- SUCCESSFUL |
| `copy` | `fresh` + `fresh` + `disjoint` | 0 of 139 -- SUCCESSFUL |

`writes` alone does not discharge. That is settled behaviour, not a bug to fix:
see (a) in section 7. What B3 owes the user is a *diagnostic* rather than ten
mystery failures.

### Extra work B3 owns, from decision (a)

`prove` must detect the shape that cannot discharge and say so. A function with
`writes`/`writes_n` on a pointer parameter and no `fresh` on that same pointer
gets a diagnostic naming the missing clause, before CBMC is ever invoked.
Without it the user sees "10 of 139 failed" and goes looking for a bug in their
own code. This is the single highest-value thing in B3 after `prove` working at
all.

### Caller mode, which the plan did not have

`--enforce-contract` verifies a function against its own contract, and there a
precondition is an *assumption*. Preconditions only become *obligations* when
verifying a caller, which is a different goto-instrument pass:

```sh
goto-instrument --replace-call-with-contract <callee> in.goto out.goto
cbmc --function <caller> out.goto
```

Confirmed working this session: with `pre (disjoint(dst, src))` on `copy`, a
caller doing `copy(a, a, 4)` fails precondition `.4` and a caller doing
`copy(a, b, 4)` passes all four; delete the clause and the aliasing caller
verifies clean. `prove` should expose this, because it is the only mode in
which most preconditions are checked at all.

**Source:** `~/git/llvm-contracts/clang/lib/Sema/SemaContracts.cpp`, 1446 lines.
Only the emitter half is wanted. The named functions:

| fork function | line | role |
|---|---|---|
| `CProverPrinter` | 154 | `PrinterHelper` that rewrites `old`/`result`/`forall` into `__CPROVER_*` |
| `printContractExpr` | 238 | one predicate → text |
| `formatCProverClause` | 258 | one clause → `__CPROVER_requires(...)` etc. |
| `printCProverContracts` | 356 | a function's clauses |
| `forEachLoopContract`, `printCProverLoopContracts` | 376, 437 | loop clauses |
| `recordDoWhileRewrite`, `findContinueTargetingLoop` | 399, 423 | CBMC cannot take a loop contract on a `do`/`continue` shape; these rewrite it |
| `emitContractHarness` | 667 | build a CBMC entry point from the preconditions |
| `findContradictoryPrecondition` | 529 | the syntactic vacuity check |
| `findConflictingHarnessFresh` | 619 | two `is_fresh` on the same buffer |
| `Sema::EmitCProverUnit` | 803 | whole-TU rewrite, the one `prove` needs |

**Do not port the checking half** (`ActOnContractClausePredicate`,
`CheckContractPostPredicate`, `DiagnoseContractVerifiability`,
`ActOnContractAssignsClause`, ...). Those are Sema callbacks that only exist
because the fork has a parser. Their *checks* are worth porting later as
standalone AST checks; their shape is not.

**Big scoping decision already made, do not undo it:** frames (`assigns`) and
loop contracts **do not travel through the stock-clang target**. They expand to
nothing there. `prove` gets them by preprocessing the same source with
`-DC_CONTRACTS_CPROVER`, which is what `~/git/llvm-contracts/proofs/verify-contract.sh`
already does. The reason is in section 7 below and it is not negotiable without
a different design.

So `c-contracts prove` is, in the first instance, a wrapper around the existing
pipeline:

```
preprocess with -DC_CONTRACTS_CPROVER  ->  goto-cc  ->
goto-instrument --enforce-contract  ->  cbmc
```

Read `proofs/verify-contract.sh` first. It already works. The port's job is to
make it a subcommand with a harness story, not to reinvent it.

**Harness policy** (decided, per the user's "not sure" + my recommendation):
auto-generate from the preconditions by default, write the generated driver to
disk so it can be copied and hand-edited, and let `proofs/<fn>.proof.c` on disk
win over the generated one. `--bound name=N` for the unbounded pointer-size
parameters that auto-generation cannot guess.

**Vacuity gate — DECIDED 2026-09-09: on by default.** `prove` runs each harness
twice: once for real, once with `assert(0)` appended. `assert(0)` is reachable
by construction, so run 2 *must* report FAILED; if it reports SUCCESSFUL the
preconditions are unsatisfiable, CBMC proved nothing, and `prove` must say so
rather than print a green line.

`--no-vacuity` opts out. It is on by default because the failure it catches is
silent and permanent: a suite that passes while proving nothing looks exactly
like a suite that works, forever. The fork has only the *syntactic* check
(`findContradictoryPrecondition`, `test/Sema/c-contracts-harness-contradiction.c`),
which catches literal contradictions on one variable; the interesting ones are
semantic (`pre(fresh(p, n))` with an `n` the harness cannot allocate) and only
appear under the solver.

Cost, so it is not a surprise: this doubles solver time, and `proofs/zstd/COST.md`
records up to 20x between solvers on the same goto binary. Budget against e2e
case 7, which holds a proof to 60s. If that hurts, the escape hatch to build
next is caching the vacuity verdict per function keyed on the contract text, so
the second run only happens when a contract changes -- not turning the gate off.

**Done for the vacuity half looks like:** a fixture whose preconditions are
semantically contradictory, on which `prove` exits non-zero and names the
vacuity, plus a gate audit confirming that removing the check makes that same
fixture report success.

**Done looks like:** `c-contracts prove <fn> <file>` reaching VERIFICATION
SUCCESSFUL on `test/cases/`-style fixtures, plus a differential gate: the
emitted CBMC text matches the fork's `-fcontract-emit-cprover-unit` byte for
byte on the fork's existing lit fixtures. That differential is the whole reason
the fork is still alive; build it early, not last.

**Suggested order**, smallest provable step first:

1. `prove` as a thin wrapper over `verify-contract.sh`'s existing pipeline, one
   function, one file, no harness generation. It already works; make it a
   subcommand.
2. The differential gate against `-fcontract-emit-cprover-unit`. Cheap once (1)
   exists, and it is what stops the emitter drifting.
3. The `writes`-without-`fresh` diagnostic. Highest user value per line.
4. Harness generation from preconditions, with `proofs/<fn>.proof.c` on disk
   winning over the generated one.
5. The vacuity gate.
6. Caller mode (`--replace-call-with-contract`).

**Budget it.** CBMC runs are minutes, and the vacuity gate doubles that. Do not
put a full proof in `test/run.sh`, which is a fast fixture runner -- proofs need
their own target with their own timeout, and e2e case 7 in the fork holds a
proof to 60s as the reference for what is affordable.

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
