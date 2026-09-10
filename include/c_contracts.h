/*===-- c_contracts.h - C contracts portable annotation layer -------------===
 *
 * Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
 * See https://llvm.org/LICENSE.txt for license information.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 *
 *===----------------------------------------------------------------------===
 *
 * Portable contract annotations for C. Three targets, chosen at include time:
 *
 *   CBMC            -DC_CONTRACTS_CPROVER: __CPROVER_requires etc.
 *   stock clang     diagnose_if: preconditions checked at every call site
 *   everything else nothing at all
 *
 * Annotated source stays buildable by GCC, MSVC, tcc and stock clang, at any
 * standard level from C89 on. No variadic macros, no includes.
 *
 * See README.md for the full reference.
 *
 *===----------------------------------------------------------------------===
 */

#ifndef C_CONTRACTS_H
#define C_CONTRACTS_H

/* Bumped when the annotation language changes visibly. */
#define C_CONTRACTS_VERSION 4

/* Suppresses -Wunused-variable for variables that only appear in annotations.
 * Sits in declarator position -- `BYTE* const opStart contract_ghost = op;` --
 * so it has to be an attribute. MSVC has no attribute that works there and
 * __pragma is not valid mid-declarator, so MSVC gets nothing. */
#if defined(__GNUC__) || defined(__clang__)
#define contract_ghost __attribute__((unused))
#else
#define contract_ghost
#endif

/* Target selection. */
#ifdef __has_attribute
#if __has_attribute(diagnose_if) && !defined(C_CONTRACTS_CPROVER) &&           \
    !defined(C_CONTRACTS_STOCK)
#define C_CONTRACTS_STOCK 1
#endif
#endif

#ifndef C_CONTRACTS_STOCK
#define C_CONTRACTS_STOCK 0
#endif

/* ---- CBMC target ---- */
#ifdef C_CONTRACTS_CPROVER

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

#define contract_writes_nothing() __CPROVER_assigns()
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
#define contract_disjoint(P, Q) (!__CPROVER_same_object((P), (Q)))
#define contract_pointer_offset(P) __CPROVER_POINTER_OFFSET(P)
#define contract_old(E) __CPROVER_old(E)
#define contract_result __CPROVER_return_value
#define contract_ssize_t __CPROVER_ssize_t
#define contract_range(P, LO, HI)                                                     \
  __CPROVER_object_upto((P) + (LO), ((HI) - (LO)) * sizeof(*(P)))
#define contract_forall(I, LO, HI, P)                                                 \
  __CPROVER_forall { unsigned long I; ((I) >= (LO) && (I) < (HI)) ==> (P) }
#define contract_exists(I, LO, HI, P)                                                 \
  __CPROVER_exists { unsigned long I; ((I) >= (LO) && (I) < (HI)) && (P) }
#define contract_frees(L) __CPROVER_frees(L)
#define contract_freeable(P) __CPROVER_is_freeable(P)
#define contract_was_freed(P) __CPROVER_was_freed(P)
#define contract_loop_entry(E) __CPROVER_loop_entry(E)
#define contract_object_whole(P) __CPROVER_object_whole(P)
#define contract_object_from(P) __CPROVER_object_from(P)
#define contract_obeys(F, C) __CPROVER_obeys_contract((F), (C))

/* ---- stock clang target ----
 * Preconditions become diagnose_if warnings at every call site.
 * Postconditions and frames ride along as annotate strings for tooling.
 * Loop contracts expand to nothing (they reach CBMC via -DC_CONTRACTS_CPROVER).
 */
#elif C_CONTRACTS_STOCK

/* Deliberately not push/pop'd. -Wgcc-compat fires where diagnose_if is
 * WRITTEN, which is in the including file, so popping at the end of this
 * header would put it back before a single annotated declaration is compiled
 * and cost the project three warnings per annotation. Including this file is
 * the request for the extension. */
#pragma clang diagnostic ignored "-Wgcc-compat"

/* Declared for type-checking only; never defined or called. */
int __contract_readable(const void *, unsigned long);
int __contract_writable(const void *, unsigned long);
int __contract_fresh(const void *, unsigned long);
int __contract_same_object(const void *, const void *);
long __contract_pointer_offset(const void *);
int __contract_freeable(const void *);
int __contract_was_freed(const void *);
int __contract_obeys(void (*)(void), void (*)(void));

/* diagnose_if fires when the condition holds, so negate the contract. */
#define contract_pre(P)                                                               \
  __attribute__((diagnose_if(!(P),                                             \
                             "precondition " #P " is violated by this "        \
                             "call",                                           \
                             "warning")))

#ifdef C_CONTRACTS_NO_INPLACE_POST
#define contract_post(P) __attribute__((annotate("contract_post:" #P)))
#else
/* 0 && (P) never fires but still type-checks the expression. */
#define contract_post(P)                                                              \
  __attribute__((annotate("contract_post:" #P)))                                      \
  __attribute__((diagnose_if(0 && (P), "postcondition " #P, "warning")))
#endif
#define contract_returns(P) __attribute__((annotate("contract_returns:" #P)))

/* In prototype scope, parameters ARE the entry values. */
#define contract_old(E) (E)

#define contract_assigns(L)
#define contract_frees(L)
#define contract_writes_nothing()
#define contract_locations(A, B) A, B

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
#define contract_freeable(P) __contract_freeable(P)
#define contract_was_freed(P) __contract_was_freed(P)
#define contract_obeys(F, C) __contract_obeys((F), (C))
#define contract_ssize_t long
#define contract_range(P, LO, HI) (P)[(LO) : (HI)]

/* Stock clang has no quantifier syntax; these degrade to true. */
#define contract_forall(I, LO, HI, P) 1
#define contract_exists(I, LO, HI, P) 1

#define contract_invariant(P)
#define contract_decreases(M)


/* ---- strip target (GCC, MSVC, tcc, or clang with -DC_CONTRACTS_STOCK=0) ---- */
#else

#define contract_reads(P, N)
#define contract_writes(P, N)
#define contract_reads_n(P, N)
#define contract_writes_n(P, N)
#define contract_writes_nothing()
#define contract_returns(P)
#define contract_forall(I, LO, HI, P)
#define contract_pre(P)
#define contract_post(P)
#define contract_assigns(L)
#define contract_frees(L)
#define contract_exists(I, LO, HI, P)
#define contract_invariant(P)
#define contract_decreases(M)

#endif

#endif
