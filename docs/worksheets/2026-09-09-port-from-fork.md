# Worksheet: contracts for C without a clang fork

Started 2026-09-09. Status: phase 1 and the tool skeleton are done and
committed; the three ports below are not started.

A fresh agent should be able to finish from this file alone. Read it, then read
`README.md` for what the thing is, then start at "Next: B2".

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

## 3. What is done

### Fork: `llvm-contracts` commit `04ebeaa82dfe`

`clang/lib/Headers/c_contracts.h` gained a fourth target, selected when
`__has_attribute(diagnose_if)` and no other target claimed the file.
`clang/test/Sema/c-contracts-macro-layer.c` updated: it asserted the header
vanishes without `-fc-contracts`, which is what we deliberately changed.

Gate: 35/35 lit.

```sh
cd ~/git/llvm-contracts
ninja -C build-arm clang clang-resource-headers
./build-arm/bin/llvm-lit -q clang/test/{Sema,Parser,CodeGen,Driver}/c-contracts*.c \
  clang/test/PCH/c-contracts.c clang/test/Preprocessor/feature-c-contracts.c \
  clang/test/Frontend/c-contracts-cxx-rejected.cpp
```

`ninja clang-resource-headers` is **not optional** after touching the header:
lit reads the copy under `build-arm/lib/clang/24/include/`, and a stale copy
produces failures that have nothing to do with your change.

### This repo: commit `4695e69`

| file | lines | does |
|---|---|---|
| `CMakeLists.txt` | 40 | `find_package(Clang)`; `project(... C CXX)` because LLVMConfig probes libedit with `check_include_file` |
| `src/Contract.h` | 99 | `ClauseKind`, `Clause`, `Contract`, and the two entry points |
| `src/Extract.cpp` | 132 | `DiagnoseIfAttr` → Pre, `AnnotateAttr` `"c_post:"`/`"c_returns:"` → the rest |
| `src/Ghost.cpp` | 284 | synthesis, reparse, diagnostic remapping |
| `src/main.cpp` | 187 | `--list`, `--warnings-as-errors`, exit status |
| `test/run.sh` + 3 cases | | fixture runner, `UPDATE=1` to rebless, filter arg |

Build and gates:

```sh
cd ~/git/c-contracts
cmake -G Ninja -B build -DCMAKE_PREFIX_PATH=$(brew --prefix llvm)
ninja -C build && ./test/run.sh build/c-contracts && ./tools/sync-header.sh
```

Working end to end:

```
$ ./build/c-contracts t.c -- -std=c89 -Iinclude
t.c:8:36:  error: use of undeclared identifier 'dstCapp'; did you mean 'dstCap'?
t.c:11:16: error: use of undeclared identifier 'c_result'
t.c:15:19: error: invalid operands to binary expression ('typeof (c(n))' (aka 'struct S') and 'int')
```

## 4. Next: B2 — the call-site dataflow pass

**Why it is needed even though `diagnose_if` exists.** Clang only fires when the
condition folds against the actual argument *expressions*. `int n = 0;
allocate(n);` folds nothing. The fork's pass is a CFG dataflow that tracks
variables and catches it. That is the whole of level 2's value over level 1.

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

**Vacuity gate — argued for, not yet decided by the user.** `prove` must run
each harness twice: once for real, once with `assert(0)` appended. If run 2 also
reports SUCCESSFUL, the preconditions are contradictory and the proof is
worthless. Without this a proof suite can go green while proving nothing, which
is the "gate stuck at pass" failure mode. The fork has the *syntactic* check
(`findContradictoryPrecondition`, and `test/Sema/c-contracts-harness-contradiction.c`);
the semantic one only shows up under the solver.

**Done looks like:** `c-contracts prove <fn> <file>` reaching VERIFICATION
SUCCESSFUL on `test/cases/`-style fixtures, plus a differential gate: the
emitted CBMC text matches the fork's `-fcontract-emit-cprover-unit` byte for
byte on the fork's existing lit fixtures.

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
target. Lit: 35/35, and the new CHECK was perturbed to confirm it fails when it
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

**(b) Vacuity gate in v1?** My argument for yes is in section 5.

**(c) Postconditions on the declaration.** They currently live on the
declaration as annotate markers and are checked in a ghost, which works. The
*fork* additionally type-checks them in place. If that gap ever matters, the fix
is a ~60-line upstream patch exposing `ParseArgsInFunctionScope` (and a result
binding) to plugin attributes via `ParsedAttrInfo` — `ParseDecl.cpp:112`'s
`.Default(false)` is the whole obstacle. Nice-to-have, blocks nothing.

## 8. Not verified locally

**Real GCC.** No gcc on this machine; `/usr/bin/gcc` is Apple clang. The
stock-clang branch is guarded by `__has_attribute(diagnose_if)`, which GCC
answers 0 to, so it is safe by construction — but construction is not a test.
Someone with a real gcc should run `gcc -std=c89 -pedantic -Wall -Wextra` over
`test/cases/*.c` and confirm silence. Same for MSVC and tcc, which the fork's
README also claims.
