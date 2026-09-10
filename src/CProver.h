//===- CProver.h - Rendering a contract the way CBMC spells it ------------===//
//
/// \file
/// The half of the fork's SemaContracts.cpp that `prove` needs.
///
/// Most of the lowering is already done by the time this runs: preprocessing
/// the same source with -DC_CONTRACTS_CPROVER turns every clause into
/// __CPROVER_requires / __CPROVER_assigns / __CPROVER_loop_invariant, with no
/// compiler in the pipeline that understands contracts. What is left is the
/// entry point, which cannot come from a macro because it needs the parameter
/// *types* -- and those live in the AST stock clang already built.
//
//===----------------------------------------------------------------------===//

#ifndef C_CONTRACTS_CPROVER_H
#define C_CONTRACTS_CPROVER_H

#include "Contract.h"

#include "llvm/ADT/StringMap.h"

#include <string>
#include <vector>

namespace ccontracts {

/// A pointer a contract claims is valid to touch without saying how big the
/// object behind it is.
struct MissingFresh {
  std::string Pointer; ///< As the predicate names it.
  std::string Size;    ///< How much of it the clause claimed.
  std::string Clause;  ///< The clause that made the claim, as written.
  bool IsWrite;        ///< writable rather than readable.
  clang::SourceLocation Loc;
};

/// Finds every `writable(p, n)` / `readable(p, n)` precondition with no
/// `fresh(p, ...)` beside it.
///
/// This is the shape that cannot discharge: `w_ok` says the memory is valid to
/// write and deliberately says nothing about which object it belongs to, so a
/// verifier handed an otherwise unconstrained pointer has no object to reason
/// about and reports a fistful of failures that look like bugs in the user's
/// code. Naming the missing clause up front is worth more than the ten
/// mystery failures it replaces.
std::vector<MissingFresh> findWritesWithoutFresh(const Contract &C);

/// The generated entry points for \p C, as C text to append to the
/// CPROVER-preprocessed translation unit.
struct Harness {
  std::string Text;
  /// The vacuity probe, which has to be compiled into a translation unit of
  /// its own: CBMC checks every assertion in the goto program it is given, not
  /// only the ones its entry point can reach, so a probe that asserts false
  /// would fail every other proof built from the same binary.
  std::string VacuityText;
  /// True if any allocation's extent is not a constant. CBMC's built-in SAT
  /// backend has to bit-blast such an extent; an SMT solver with a theory of
  /// arrays does not. COST.md measures up to 20x, in either direction, so the
  /// choice is made from this rather than guessed.
  bool SymbolicExtents = false;
  /// Diagnostics the synthesis produced: an allocation whose size is a
  /// parameter nothing has bounded yet.
  std::vector<std::string> Warnings;
};

/// Builds a CBMC entry point for \p C from its preconditions, plus a vacuity
/// probe that runs the same assumptions and then asserts false.
///
/// `fresh(L, N)` allocates; every other conjunct is assumed. A parameter no
/// clause mentions is left uninitialised, which is nondeterministic in CBMC --
/// the honest default, since the contract said nothing about it.
///
/// \p Bounds caps a parameter the contract leaves unbounded (`--bound n=64`).
/// Returns false and sets \p Err if the contract cannot produce one.
bool emitHarness(const Contract &C, llvm::StringRef HarnessName,
                 llvm::StringRef VacuityName,
                 const llvm::StringMap<std::string> &Bounds, Harness &Out,
                 std::string &Err);

/// Every __CPROVER_ clause in \p Text, in file order.
///
/// \p Canonical removes whitespace and parentheses. Two renderings of the same
/// clause -- this tool's macro expansion and the fork's AST printer -- differ
/// in exactly those, and in nothing else that is not a real drift, so that is
/// what the differential gate compares. Both sides go through this one
/// function, so the gate cannot be passed by a canonicaliser that disagrees
/// with itself.
std::vector<std::string> extractClauses(llvm::StringRef Text, bool Canonical);

} // namespace ccontracts

#endif // C_CONTRACTS_CPROVER_H
