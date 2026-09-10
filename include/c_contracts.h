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
 * No variadic macros are used, so -std=c89 -pedantic stays quiet. c_locations
 * combines two frame locations and nests when a frame needs more.
 *
 *===----------------------------------------------------------------------===
 */

#ifndef __C_CONTRACTS_H
#define __C_CONTRACTS_H

/* Marks a declaration that exists only to be named by an annotation. Such a
 * variable is genuinely unused once the annotations vanish, so without this
 * every annotated loop that needs a starting-value witness costs the project a
 * -Wunused-variable warning, and a -Werror build refuses to compile:
 *
 *   BYTE* const opStart c_ghost = op;
 */
#if defined(__GNUC__) || defined(__clang__)
#define c_ghost __attribute__((unused))
#else
#define c_ghost
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
    !defined(C_CONTRACTS_CPROVER)
#define C_CONTRACTS_STOCK 1
#endif
#endif

#ifndef C_CONTRACTS_STOCK
#define C_CONTRACTS_STOCK 0
#endif

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
 * Everything in the language survives this mode, but only through the c_ names.
 * The bare spellings a contract-aware front end also accepts -- readable, old,
 * result, a p[lo : hi] range, forall (i : lo, hi) P -- are grammar rather than
 * macros, and nothing discards them here. Write c_readable, c_same_object,
 * c_pointer_offset, c_old, c_result, c_range and c_forall in source that has to
 * reach CBMC directly.
 */
#ifdef C_CONTRACTS_CPROVER
#undef C_CONTRACTS
#define C_CONTRACTS 0

#define c_reads(P, N)                                                          \
  __CPROVER_requires((P) != 0) __CPROVER_requires(__CPROVER_r_ok((P), (N)))
#define c_writes(P, N)                                                         \
  __CPROVER_requires((P) != 0) __CPROVER_requires(__CPROVER_w_ok((P), (N)))     \
      __CPROVER_assigns(__CPROVER_object_upto(((char *)(P)), (N)))
#define c_reads_n(P, N)                                                        \
  __CPROVER_requires((P) != 0)                                                 \
      __CPROVER_requires(__CPROVER_r_ok((P), (N) * sizeof(*(P))))
#define c_writes_n(P, N)                                                       \
  __CPROVER_requires((P) != 0)                                                 \
      __CPROVER_requires(__CPROVER_w_ok((P), (N) * sizeof(*(P))))              \
          __CPROVER_assigns(__CPROVER_object_upto((P), (N) * sizeof(*(P))))

#define c_writes_nothing __CPROVER_assigns()
#define c_returns(P) __CPROVER_ensures(P)

#define c_pre(P) __CPROVER_requires(P)
#define c_post(P) __CPROVER_ensures(P)
#define c_assigns(L) __CPROVER_assigns(L)
#define c_locations(A, B) A, B
#define c_invariant(P) __CPROVER_loop_invariant(P)
#define c_decreases(M) __CPROVER_decreases(M)

#define c_readable(P, N) __CPROVER_r_ok((P), (N))
#define c_writable(P, N) __CPROVER_w_ok((P), (N))
#define c_fresh(P, N) __CPROVER_is_fresh((P), (N))
#define c_same_object(P, Q) __CPROVER_same_object((P), (Q))
#define c_pointer_offset(P) __CPROVER_POINTER_OFFSET(P)
#define c_old(E) __CPROVER_old(E)
#define c_result __CPROVER_return_value
#define c_ssize_t __CPROVER_ssize_t
#define c_range(P, LO, HI)                                                     \
  __CPROVER_object_upto((P) + (LO), ((HI) - (LO)) * sizeof(*(P)))
#define c_forall(I, LO, HI, P)                                                 \
  __CPROVER_forall { unsigned long I; ((I) >= (LO) && (I) < (HI)) ==> (P) }

#elif C_CONTRACTS

/* Roles: what the function does to a buffer. A role is the spelling to reach
 * for; the clauses below are what it lowers to. c_writes covers both the
 * caller's obligation to supply the memory and the promise that nothing outside
 * it changes, because a function that writes a buffer always means both.
 *
 * There are two roles, not three. A function that reads a buffer and then
 * writes it carries both, and the difference that matters is stated by their
 * combination rather than by a third name: c_reads is what obliges the caller
 * to have initialized the memory. memset only writes; buf[i] *= 2 does both.
 *
 * A count is in BYTES, matching memcpy and every C interface that pairs a
 * void * with a size. The _n forms count ELEMENTS of a typed pointer.
 */
#define c_reads(P, N) pre((P) != 0) pre(readable((P), (N)))
#define c_writes(P, N)                                                         \
  pre((P) != 0) pre(writable((P), (N))) assigns(((char *)(P))[0 : (N)])

#define c_reads_n(P, N) pre((P) != 0) pre(readable((P), (N) * sizeof(*(P))))
#define c_writes_n(P, N)                                                       \
  pre((P) != 0) pre(writable((P), (N) * sizeof(*(P)))) assigns((P)[0 : (N)])

/* The function writes nothing a caller can observe. An annotation with no write
 * role and no c_assigns makes no claim about the frame at all, so a pure reader
 * needs this to say so.
 */
#define c_writes_nothing assigns()

/* The result, under a fixed name, so nothing has to be bound by hand. */
#define c_returns(P) post(result : P)

/* Prefixed spellings of the predicates that appear inside a clause. Under a
 * contract-aware front end these are the keywords and intrinsics themselves, so
 * the short names work too; the c_ forms are what a translation unit targeting
 * CBMC directly has to use, since nothing discards them there.
 */
#define c_readable(P, N) readable((P), (N))
#define c_writable(P, N) writable((P), (N))
#define c_fresh(P, N) fresh((P), (N))
#define c_same_object(P, Q) same_object((P), (Q))
#define c_pointer_offset(P) pointer_offset(P)
#define c_old(E) old(E)
#define c_result result
#define c_ssize_t long
#define c_range(P, LO, HI) (P)[(LO) : (HI)]
#define c_forall(I, LO, HI, P) forall(I : LO, HI)(P)

/* The primitive layer. Reach for these when a role cannot say it: a global in
 * the frame, a partial write, a relation between two parameters.
 */
#define c_pre(P) pre(P)
#define c_post(P) post(P)
#define c_assigns(L) assigns(L)
#define c_locations(A, B) A, B
#define c_invariant(P) loop_invariant(P)
#define c_decreases(M) decreases(M)

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
int __c_readable(const void *, unsigned long);
int __c_writable(const void *, unsigned long);
int __c_fresh(const void *, unsigned long);
int __c_same_object(const void *, const void *);
long __c_pointer_offset(const void *);

/* diagnose_if fires when its condition holds, so the condition is the negation
 * of the contract. #P quotes the clause as the user spelled it, before any
 * project macro in it expands, which is what a reader wants to see named.
 */
#define c_pre(P)                                                               \
  __attribute__((diagnose_if(!(P),                                             \
                             "precondition " #P " is violated by this "        \
                             "call",                                           \
                             "warning")))

/* A postcondition is one expression, so it survives quoting and can be
 * type-checked later in a scope the tool builds. `#P` deliberately does not
 * expand: a project macro inside the clause is re-expanded, in this same
 * translation unit, when the tool reparses it, which is the only context where
 * it means the right thing.
 */
#define c_post(P) __attribute__((annotate("c_post:" #P)))
#define c_returns(P) __attribute__((annotate("c_returns:" #P)))

/* Frames are not expressions and do not survive quoting: `locations(a, b)` and
 * `range(p, lo, hi)` are macros whose expansion depends on the target, and a
 * frame reaching the tool as text would have to be expanded in a context that
 * does not exist here. Frames and loop contracts reach the verifier the way
 * they always have, by preprocessing the same source with
 * -DC_CONTRACTS_CPROVER, so this target drops them.
 */
#define c_assigns(L)
#define c_writes_nothing
#define c_locations(A, B) A, B

/* Roles, split by who can check them: the caller's obligation is a
 * precondition clang folds at the call site, the frame is for the verifier.
 */
#define c_reads(P, N) c_pre((P) != 0) c_pre(c_readable((P), (N)))
#define c_writes(P, N) c_pre((P) != 0) c_pre(c_writable((P), (N)))

#define c_reads_n(P, N)                                                        \
  c_pre((P) != 0) c_pre(c_readable((P), (N) * sizeof(*(P))))
#define c_writes_n(P, N)                                                       \
  c_pre((P) != 0) c_pre(c_writable((P), (N) * sizeof(*(P))))

#define c_readable(P, N) __c_readable((P), (N))
#define c_writable(P, N) __c_writable((P), (N))
#define c_fresh(P, N) __c_fresh((P), (N))
#define c_same_object(P, Q) __c_same_object((P), (Q))
#define c_pointer_offset(P) __c_pointer_offset(P)
#define c_ssize_t long
#define c_range(P, LO, HI) (P)[(LO) : (HI)]

/* A quantified precondition is not something a call site can fold, and stock
 * clang has no syntax that binds the variable. Standing in for it with a true
 * literal keeps the declaration compiling and keeps the clause from ever
 * firing; the quantifier itself reaches the verifier through the CPROVER
 * target, which does have the syntax.
 */
#define c_forall(I, LO, HI, P) 1

/* Loop contracts: see the note at the top of this branch. */
#define c_invariant(P)
#define c_decreases(M)

#else

#define c_reads(P, N)
#define c_writes(P, N)
#define c_reads_n(P, N)
#define c_writes_n(P, N)
#define c_writes_nothing
#define c_returns(P)
#define c_forall(I, LO, HI, P)
#define c_pre(P)
#define c_post(P)
#define c_assigns(L)
#define c_invariant(P)
#define c_decreases(M)

#endif


#ifdef __cplusplus
extern "C" void __contract_violation(const char *predicate, const char *file,
                                     unsigned line, const char *function);
#else
void __contract_violation(const char *predicate, const char *file,
                          unsigned line, const char *function);
#endif

#endif

/* Deliberately OUTSIDE the include guard above, and with a guard of its own.
 * A project that vendors this header will often see it included first by some
 * other header that does not want the unprefixed spelling; if these aliases sat
 * inside the main guard, a later
 *
 *     #define C_CONTRACTS_NO_PREFIX
 *     #include "c_contracts.h"
 *
 * would be a no-op and every unprefixed clause would fail to compile with
 * "call to undeclared function 'range'". Include order decided the language,
 * which is not a property an annotation layer may have.
 */
#ifdef C_CONTRACTS_NO_PREFIX
#ifndef __C_CONTRACTS_NO_PREFIX_H
#define __C_CONTRACTS_NO_PREFIX_H
/* Unprefixed spelling, opt-in with C_CONTRACTS_NO_PREFIX.
 *
 * Each alias forwards to its c_ form, so this works the same under every
 * target: expanding pre(P) yields c_pre(P), which yields the right thing for
 * the target, and the rescan leaves the inner `pre` alone because a macro is
 * not replaced inside its own expansion.
 *
 * Only function-like names appear here, and that is the whole rule. A
 * function-like macro expands only where the name is followed by `(`, so a
 * project keeps `int pre;`, `s.post`, and a field called `range`. The four
 * object-like names -- c_result, c_ssize_t, c_ghost, c_writes_nothing -- would
 * rewrite every bare occurrence of a very common word, so they are never
 * unprefixed. Check a candidate project first: grep for `name(` rather than for
 * the bare word.
 */

/* These six are real grammar under a contract-aware front end, so aliasing them
 * there would only shadow the keyword and earn a -Wc-contracts warning per
 * translation unit. Elsewhere they have to be macros.
 */
#if !C_CONTRACTS
#define pre(P) c_pre(P)
#define post(P) c_post(P)
/* Not `c_assigns(L)`. A frame is written `assigns(locations(a, b))`, and
 * forwarding would expand `locations` during the argument prescan, handing the
 * next macro three arguments where it declared one. `assigns` has to be the
 * last function-like macro in the chain, so each target spells it out.
 */
#ifdef C_CONTRACTS_CPROVER
#define assigns(L) __CPROVER_assigns(L)
#else
#define assigns(L)
#endif
#define loop_invariant(P) c_invariant(P)
#define decreases(M) c_decreases(M)
#define old(E) c_old(E)
#endif

/* The rest are macros under every target, so they alias unconditionally. */
#define locations(A, B) A, B
#define returns(P) c_returns(P)
#define reads(P, N) c_reads(P, N)
#define writes(P, N) c_writes(P, N)
#define reads_n(P, N) c_reads_n(P, N)
#define writes_n(P, N) c_writes_n(P, N)
#define readable(P, N) c_readable(P, N)
#define writable(P, N) c_writable(P, N)
#define fresh(P, N) c_fresh(P, N)
#define same_object(P, Q) c_same_object(P, Q)
#define pointer_offset(P) c_pointer_offset(P)
#define range(P, LO, HI) c_range(P, LO, HI)

/* No unprefixed forall. The keyword binds its variable with its own syntax,
 * forall (i : lo, hi) P, and the portable spelling takes four arguments, so one
 * name cannot serve both. Write c_forall.
 */
#endif
#endif
