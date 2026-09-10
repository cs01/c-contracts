//===- Extract.cpp - Recover contract clauses from attributes -------------===//

#include "Contract.h"

#include "clang/AST/ASTContext.h"
#include "clang/AST/Attr.h"
#include "clang/AST/Expr.h"
#include "clang/Basic/SourceManager.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/raw_ostream.h"

using namespace clang;

namespace ccontracts {

llvm::StringRef describe(ClauseKind K) {
  switch (K) {
  case ClauseKind::Pre:
    return "precondition";
  case ClauseKind::Post:
  case ClauseKind::Returns:
    return "postcondition";
  }
  llvm_unreachable("unhandled clause kind");
}

llvm::StringRef markerPrefix(ClauseKind K) {
  switch (K) {
  case ClauseKind::Pre:
    return "";
  case ClauseKind::Post:
    return "c_post:";
  case ClauseKind::Returns:
    return "c_returns:";
  }
  llvm_unreachable("unhandled clause kind");
}

bool Contract::needsGhost() const {
  for (const Clause &C : Clauses)
    if (C.Kind != ClauseKind::Pre)
      return true;
  return false;
}

namespace {

/// Every precondition c_contracts.h emits carries this message. A diagnose_if
/// written by hand means something the header did not say, so it is not read as
/// a contract.
constexpr llvm::StringLiteral PreMessagePrefix = "precondition ";
constexpr llvm::StringLiteral PreMessageSuffix = " is violated by this call";

/// diagnose_if fires on violation, so the header writes `!(P)`. Recover P.
/// Returns null if the shape is not the header's, which is the same signal as
/// the message prefix and is checked for the same reason.
const Expr *contractFromViolation(const Expr *Cond) {
  if (!Cond)
    return nullptr;
  const auto *UO = dyn_cast<UnaryOperator>(Cond->IgnoreParenImpCasts());
  if (!UO || UO->getOpcode() != UO_LNot)
    return nullptr;
  return UO->getSubExpr()->IgnoreParenImpCasts();
}

/// The predicate as the author spelled it, recovered from the message the
/// header quoted it into. Printing the AST back instead would name the
/// never-defined helpers a role lowers to -- `__c_writable(((p)), ((n)))` where
/// the source says `c_writable(p, n)` -- and a reader has to be able to find
/// what they are being told about in their own file.
llvm::StringRef spellingFromMessage(llvm::StringRef Message) {
  llvm::StringRef Text = Message.drop_front(PreMessagePrefix.size());
  Text.consume_back(PreMessageSuffix);
  return Text;
}

/// A clause reaches the AST through a macro, so the attribute's own location is
/// inside c_contracts.h. What a reader wants is the annotation they wrote.
SourceLocation clauseLoc(const Attr *A, const SourceManager &SM) {
  return SM.getExpansionLoc(A->getLocation());
}

} // namespace

Contract collectContract(const FunctionDecl *FD) {
  Contract C;
  if (!FD || !FD->hasAttrs())
    return C;

  const ASTContext &Ctx = FD->getASTContext();
  const SourceManager &SM = Ctx.getSourceManager();

  for (const Attr *A : FD->attrs()) {
    if (const auto *DI = dyn_cast<DiagnoseIfAttr>(A)) {
      if (!DI->getMessage().starts_with(PreMessagePrefix))
        continue;
      const Expr *P = contractFromViolation(DI->getCond());
      if (!P)
        continue;
      C.Clauses.push_back({ClauseKind::Pre,
                           spellingFromMessage(DI->getMessage()).str(), P,
                           clauseLoc(A, SM)});
      continue;
    }

    if (const auto *Ann = dyn_cast<AnnotateAttr>(A)) {
      llvm::StringRef Text = Ann->getAnnotation();
      for (ClauseKind K : {ClauseKind::Post, ClauseKind::Returns}) {
        llvm::StringRef Prefix = markerPrefix(K);
        if (!Text.starts_with(Prefix))
          continue;
        C.Clauses.push_back({K, Text.drop_front(Prefix.size()).str(), nullptr,
                             clauseLoc(A, SM)});
        break;
      }
    }
  }

  // Attribute order is not declaration order: clang groups the annotate
  // markers apart from the diagnose_ifs, so a `writes` clause and the `returns`
  // below it come back inverted. Source order is what the author wrote and what
  // a reader is looking for.
  llvm::stable_sort(C.Clauses, [&SM](const Clause &A, const Clause &B) {
    return SM.isBeforeInTranslationUnit(A.Loc, B.Loc);
  });

  if (!C.Clauses.empty())
    C.Fn = FD;
  return C;
}

} // namespace ccontracts
