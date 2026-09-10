/*===-- c_contracts.h - C contracts portable annotation layer -------------===
 *
 * Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
 * See https://llvm.org/LICENSE.txt for license information.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 *
 *===----------------------------------------------------------------------===
 *
 * Annotations that vanish under a compiler that does not understand them.
 *
 * This header is the annotation language. The contract grammar it expands to is
 * one of four targets, chosen at include time:
 *
 *   contract-aware front end   __has_feature(c_contracts): pre/post/assigns
 *   CBMC directly              -DC_CONTRACTS_CPROVER: __CPROVER_requires etc.
 *   stock clang                __has_attribute(diagnose_if): preconditions
 *                              checked at every call site, the rest quoted
 *                              into annotate markers for c-contracts to read
 *   everything else            nothing at all
 *
 * Annotated source stays buildable by GCC, MSVC, tcc and stock clang, at any
 * standard level from C89 on, with no second code path -- every target produces
 * the same declaration.
 *
 * This file is self-contained and intended to be vendored: copy it into a
 * project rather than depending on a particular compiler shipping it. It is
 * written in C89 with no includes so that it cannot constrain what includes it.
 *
 * No variadic macros are used, so -std=c89 -pedantic stays quiet. contract_locations
 * combines two frame locations and nests when a frame needs more.
 *
 *===----------------------------------------------------------------------===
 */

#ifndef __C_CONTRACTS_H
#define __C_CONTRACTS_H

/* This header is meant to be copied into a project and committed there, so a
 * vendored copy has to be able to say which one it is. Bumped whenever the
 * annotation language changes in a way a consumer could notice: a clause added
 * or removed, a spelling changed, a marker string changed. Not bumped for
 * comments or for a fix that leaves every expansion identical.
 *
 * A project can test it:  #if C_CONTRACTS_VERSION < 2 ... #endif
 * and c-contracts reports a copy older than the one it was built against,
 * rather than quietly finding no clauses.
 */
#define C_CONTRACTS_VERSION 2

/* Marks a declaration that exists only to be named by an annotation. Such a
 * variable is genuinely unused once the annotations vanish, so without this
 * every annotated loop that needs a starting-value witness costs the project a
 * -Wunused-variable warning, and a -Werror build refuses to compile:
 *
 *   BYTE* const opStart contract_ghost = op;
 */
#if defined(__GNUC__) || defined(__clang__)
#define contract_ghost __attribute__((unused))
#else
#define contract_ghost
#endif

#ifdef __has_feature
#if __has_feature(c_contracts)
#define C_CONTRACTS 1
#endif
#endif

#ifndef C_CONTRACTS
#define C_CONTRACTS 0
#endif

/* Stock clang understands no contract grammar, but it does understand
 * diagnose_if, whose argument is parsed in the function's own prototype scope.
 * That is the whole of what a precondition needs, so a released clang can check
 * one without any of the machinery below it. Selected only when no
 * contract-aware front end and no direct CBMC target already claimed the file.
 */
#ifdef __has_attribute
#if __has_attribute(diagnose_if) && !C_CONTRACTS &&                            \
    !defined(C_CONTRACTS_CPROVER) && !defined(C_CONTRACTS_STOCK)
#define C_CONTRACTS_STOCK 1
#endif
#endif

#ifndef C_CONTRACTS_STOCK
#define C_CONTRACTS_STOCK 0
#endif
/* Autodetection above only fires when C_CONTRACTS_STOCK is not already set, so
 * -DC_CONTRACTS_STOCK=0 turns the checking off on a compiler that would
 * otherwise get it, and lands the file on the strip branch: every clause
 * preprocesses away to the bare declaration, which is what GCC, MSVC and tcc
 * see. That is also the only way to reach that branch from a clang, so it is
 * how the strip target is tested. */

/* Define C_CONTRACTS_CPROVER before including this header to target CBMC's own
 * front end directly, with no contract-aware compiler in the pipeline:
 *
 *   goto-cc -DC_CONTRACTS_CPROVER -o f.goto f.c
 *   goto-instrument --enforce-contract f f.goto f-chk.goto
 *   cbmc --function f --pointer-check --bounds-check f-chk.goto
 *
 * The same annotated source then reaches a verifier through three independent
 * paths, which is the point of putting the language in a header.
 *
 * Everything in the language survives this mode. The bare spellings a
 * contract-aware front end also accepts -- readable, old, result, a p[lo : hi]
 * range, forall (i : lo, hi) P -- are its grammar rather than macros, and
 * nothing here discards them; the contract_ names are what a translation unit
 * targeting CBMC directly has to use.
 */
#ifdef C_CONTRACTS_CPROVER
#undef C_CONTRACTS
#define C_CONTRACTS 0

#define contract_reads(P, N)                                                          \
  __CPROVER_requires((P) != 0) __CPROVER_requires(__CPROVER_r_ok((P), (N)))
#define contract_writes(P, N)                                                         \
  __CPROVER_requires((P) != 0) __CPROVER_requires(__CPROVER_w_ok((P), (N)))     \
      __CPROVER_assigns(__CPROVER_object_upto(((char *)(P)), (N)))
#define contract_reads_n(P, N)                                                        \
  __CPROVER_requires((P) != 0)                                                 \
      __CPROVER_requires(__CPROVER_r_ok((P), (N) * sizeof(*(P))))
#define contract_writes_n(P, N)                                                       \
  __CPROVER_requires((P) != 0)                                                 \
      __CPROVER_requires(__CPROVER_w_ok((P), (N) * sizeof(*(P))))              \
          __CPROVER_assigns(__CPROVER_object_upto((P), (N) * sizeof(*(P))))

#define contract_writes_nothing __CPROVER_assigns()
#define contract_returns(P) __CPROVER_ensures(P)

#define contract_pre(P) __CPROVER_requires(P)
#define contract_post(P) __CPROVER_ensures(P)
#define contract_assigns(L) __CPROVER_assigns(L)
#define contract_locations(A, B) A, B
#define contract_invariant(P) __CPROVER_loop_invariant(P)
#define contract_decreases(M) __CPROVER_decreases(M)

#define contract_readable(P, N) __CPROVER_r_ok((P), (N))
#define contract_writable(P, N) __CPROVER_w_ok((P), (N))
#define contract_fresh(P, N) __CPROVER_is_fresh((P), (N))
#define contract_same_object(P, Q) __CPROVER_same_object((P), (Q))
/* Separation, spelled so a reader can see it. writes(p, n) says only that the
 * memory is valid to write; it deliberately claims nothing about aliasing, so a
 * function that needs two buffers not to overlap has to say which two. Object
 * level, not range level: two non-overlapping ranges inside one object are not
 * disjoint by this definition, which is the same granularity is_fresh works at.
 */
#define contract_disjoint(P, Q) (!__CPROVER_same_object((P), (Q)))
#define contract_pointer_offset(P) __CPROVER_POINTER_OFFSET(P)
#define contract_old(E) __CPROVER_old(E)
#define contract_result __CPROVER_return_value
#define contract_ssize_t __CPROVER_ssize_t
#define contract_range(P, LO, HI)                                                     \
  __CPROVER_object_upto((P) + (LO), ((HI) - (LO)) * sizeof(*(P)))
#define contract_forall(I, LO, HI, P)                                                 \
  __CPROVER_forall { unsigned long I; ((I) >= (LO) && (I) < (HI)) ==> (P) }

#elif C_CONTRACTS

/* Roles: what the function does to a buffer. A role is the spelling to reach
 * for; the clauses below are what it lowers to. contract_writes covers both the
 * caller's obligation to supply the memory and the promise that nothing outside
 * it changes, because a function that writes a buffer always means both.
 *
 * There are two roles, not three. A function that reads a buffer and then
 * writes it carries both, and the difference that matters is stated by their
 * combination rather than by a third name: contract_reads is what obliges the caller
 * to have initialized the memory. memset only writes; buf[i] *= 2 does both.
 *
 * A count is in BYTES, matching memcpy and every C interface that pairs a
 * void * with a size. The _n forms count ELEMENTS of a typed pointer.
 */
#define contract_reads(P, N) pre((P) != 0) pre(readable((P), (N)))
#define contract_writes(P, N)                                                         \
  pre((P) != 0) pre(writable((P), (N))) assigns(((char *)(P))[0 : (N)])

#define contract_reads_n(P, N) pre((P) != 0) pre(readable((P), (N) * sizeof(*(P))))
#define contract_writes_n(P, N)                                                       \
  pre((P) != 0) pre(writable((P), (N) * sizeof(*(P)))) assigns((P)[0 : (N)])

/* The function writes nothing a caller can observe. An annotation with no write
 * role and no contract_assigns makes no claim about the frame at all, so a pure reader
 * needs this to say so.
 */
#define contract_writes_nothing assigns()

/* The result, under a fixed name, so nothing has to be bound by hand. */
#define contract_returns(P) post(result : P)

/* Prefixed spellings of the predicates that appear inside a clause. Under a
 * contract-aware front end these are the keywords and intrinsics themselves, so
 * the short names work too; the c_ forms are what a translation unit targeting
 * CBMC directly has to use, since nothing discards them there.
 */
#define contract_readable(P, N) readable((P), (N))
#define contract_writable(P, N) writable((P), (N))
#define contract_fresh(P, N) fresh((P), (N))
#define contract_same_object(P, Q) same_object((P), (Q))
#define contract_disjoint(P, Q) (!same_object((P), (Q)))
#define contract_pointer_offset(P) pointer_offset(P)
#define contract_old(E) old(E)
#define contract_result result
#define contract_ssize_t long
#define contract_range(P, LO, HI) (P)[(LO) : (HI)]
#define contract_forall(I, LO, HI, P) forall(I : LO, HI)(P)

/* The primitive layer. Reach for these when a role cannot say it: a global in
 * the frame, a partial write, a relation between two parameters.
 */
#define contract_pre(P) pre(P)
#define contract_post(P) post(P)
#define contract_assigns(L) assigns(L)
#define contract_locations(A, B) A, B
#define contract_invariant(P) loop_invariant(P)
#define contract_decreases(M) decreases(M)

#elif C_CONTRACTS_STOCK

/* Stock clang. Preconditions become diagnose_if, which is the same three things
 * a contract-aware front end gives them: the parameters are in scope, the
 * predicate is type-checked, and a call whose arguments make it false is a
 * warning at the call site, under -Wuser-defined-warnings.
 *
 * The clauses a caller cannot check -- the frame and the postcondition -- ride
 * along as annotate strings. Clang only lexes those, never parses them, so a
 * frame range like p[0 : n] survives intact for c-contracts to read out of the
 * AST and type-check in a scope it builds itself.
 *
 * Loop contracts expand to nothing here. Nothing in this target consumes them:
 * they exist for the verifier, and the verifier is reached by preprocessing the
 * same source with -DC_CONTRACTS_CPROVER.
 */

/* diagnose_if is a clang extension, so -pedantic reports every use of it
 * through -Wgcc-compat -- three warnings per annotated declaration, which would
 * make the header unusable on the projects most likely to want it. Including
 * this file is the request for the extension.
 */
#pragma clang diagnostic ignored "-Wgcc-compat"

/* Declared so that a predicate naming them type-checks its arguments; never
 * defined, and never needed at link time, because diagnose_if parses its
 * argument in an unevaluated context and nothing else expands to a call.
 */
int __contract_readable(const void *, unsigned long);
int __contract_writable(const void *, unsigned long);
int __contract_fresh(const void *, unsigned long);
int __contract_same_object(const void *, const void *);
long __contract_pointer_offset(const void *);

/* diagnose_if fires when its condition holds, so the condition is the negation
 * of the contract. #P quotes the clause as the user spelled it, before any
 * project macro in it expands, which is what a reader wants to see named.
 */
#define contract_pre(P)                                                               \
  __attribute__((diagnose_if(!(P),                                             \
                             "precondition " #P " is violated by this "        \
                             "call",                                           \
                             "warning")))

/* A postcondition is one expression, so it survives quoting and can be
 * type-checked later in a scope the tool builds. `#P` deliberately does not
 * expand: a project macro inside the clause is re-expanded, in this same
 * translation unit, when the tool reparses it, which is the only context where
 * it means the right thing.
 *
 * `post` additionally gets checked here and now, with no tool in the picture.
 * A post is a result-independent fact, so everything it can name -- the
 * parameters, a file-scope declaration, a project macro -- is already in scope
 * where diagnose_if parses its argument. `0 &&` folds the condition to false so
 * it can never fire at a call site, and clang type-checks the operand anyway,
 * which is the whole point: a typo in a postcondition becomes an error from a
 * plain `clang -c`, the same as one in a precondition.
 *
 * `returns` cannot join it. Binding a name to the function's own return type
 * needs a declaration, so it needs a statement expression, and a statement
 * expression inside a late-parsed attribute argument crashes clang (checked on
 * 22.1.8 and on trunk). `returns` stays marker-only and is the tool's job.
 *
 * Define C_CONTRACTS_NO_INPLACE_POST to drop the in-place check. The one shape
 * it rejects that the tool accepts is a post naming a file-scope declaration
 * that appears LATER in the translation unit: the tool reparses with the whole
 * unit in scope, this sees only what precedes the annotated declaration.
 */
#ifdef C_CONTRACTS_NO_INPLACE_POST
#define contract_post(P) __attribute__((annotate("contract_post:" #P)))
#else
#define contract_post(P)                                                              \
  __attribute__((annotate("contract_post:" #P)))                                      \
  __attribute__((diagnose_if(0 && (P), "postcondition " #P, "warning")))
#endif
#define contract_returns(P) __attribute__((annotate("contract_returns:" #P)))

/* In a scope whose parameters ARE the entry values, old(E) is E. That is what
 * makes the in-place check above possible at all, and it is the same identity
 * the tool's ghost preamble relies on. The quoted marker is unaffected: #P does
 * not expand, so the tool still sees `old(...)` and rebinds it itself.
 */
#define contract_old(E) (E)

/* Frames are not expressions and do not survive quoting: `locations(a, b)` and
 * `range(p, lo, hi)` are macros whose expansion depends on the target, and a
 * frame reaching the tool as text would have to be expanded in a context that
 * does not exist here. Frames and loop contracts reach the verifier the way
 * they always have, by preprocessing the same source with
 * -DC_CONTRACTS_CPROVER, so this target drops them.
 */
#define contract_assigns(L)
#define contract_writes_nothing
#define contract_locations(A, B) A, B

/* Roles, split by who can check them: the caller's obligation is a
 * precondition clang folds at the call site, the frame is for the verifier.
 */
#define contract_reads(P, N) contract_pre((P) != 0) contract_pre(contract_readable((P), (N)))
#define contract_writes(P, N) contract_pre((P) != 0) contract_pre(contract_writable((P), (N)))

#define contract_reads_n(P, N)                                                        \
  contract_pre((P) != 0) contract_pre(contract_readable((P), (N) * sizeof(*(P))))
#define contract_writes_n(P, N)                                                       \
  contract_pre((P) != 0) contract_pre(contract_writable((P), (N) * sizeof(*(P))))

#define contract_readable(P, N) __contract_readable((P), (N))
#define contract_writable(P, N) __contract_writable((P), (N))
#define contract_fresh(P, N) __contract_fresh((P), (N))
#define contract_same_object(P, Q) __contract_same_object((P), (Q))
#define contract_disjoint(P, Q) (!__contract_same_object((P), (Q)))
#define contract_pointer_offset(P) __contract_pointer_offset(P)
#define contract_ssize_t long
#define contract_range(P, LO, HI) (P)[(LO) : (HI)]

/* A quantified precondition is not something a call site can fold, and stock
 * clang has no syntax that binds the variable. Standing in for it with a true
 * literal keeps the declaration compiling and keeps the clause from ever
 * firing; the quantifier itself reaches the verifier through the CPROVER
 * target, which does have the syntax.
 */
#define contract_forall(I, LO, HI, P) 1

/* Loop contracts: see the note at the top of this branch. */
#define contract_invariant(P)
#define contract_decreases(M)

#else

#define contract_reads(P, N)
#define contract_writes(P, N)
#define contract_reads_n(P, N)
#define contract_writes_n(P, N)
#define contract_writes_nothing
#define contract_returns(P)
#define contract_forall(I, LO, HI, P)
#define contract_pre(P)
#define contract_post(P)
#define contract_assigns(L)
#define contract_invariant(P)
#define contract_decreases(M)

#endif


#ifdef __cplusplus
extern "C" void __contract_violation(const char *predicate, const char *file,
                                     unsigned line, const char *function);
#else
void __contract_violation(const char *predicate, const char *file,
                          unsigned line, const char *function);
#endif

#endif
