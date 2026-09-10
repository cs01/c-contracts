//===- CallSite.h - Precondition checking at call sites ---------*- C++ -*-===//
//
/// \file
/// A CFG dataflow pass that checks a callee's preconditions at each call site.
///
/// Clang's own diagnose_if only fires when the condition folds against the
/// argument *expressions*, so `int n = 0; allocate(n);` folds nothing. This
/// pass tracks locals across the CFG and catches exactly that case, which is
/// the whole of what it adds over the released compiler.
///
/// The pass is unsound and incomplete by construction. It reports definite
/// violations and simple integer ranges that do not imply a callee's bound;
/// other unknown values remain silent.
//
//===----------------------------------------------------------------------===//

#ifndef C_CONTRACTS_CALLSITE_H
#define C_CONTRACTS_CALLSITE_H

#include "Contract.h"

namespace clang {
class AnalysisDeclContext;
class CallExpr;
} // namespace clang

namespace ccontracts {

/// Receives the contract problems the call-site pass finds.
class ContractViolationReporter {
public:
  virtual ~ContractViolationReporter() = default;

  /// \p Call passes an argument that violates \p Clause, a precondition of
  /// \p Callee.
  virtual void reportPreconditionViolated(const clang::CallExpr *Call,
                                          const clang::FunctionDecl *Callee,
                                          const Clause &C) = 0;

  /// An integer range at the call site does not imply the callee's bound.
  /// Deliberately separate from the above because it is noisier.
  virtual void
  reportPreconditionNotGuaranteed(const clang::CallExpr *Call,
                                  const clang::FunctionDecl *Callee,
                                  const Clause &C) = 0;
};

/// Runs precondition checking over the body in \p AC.
void runCallSiteChecking(clang::AnalysisDeclContext &AC,
                         ContractViolationReporter &Reporter);

} // namespace ccontracts
#endif
