//===- CProver.cpp - Rendering a contract the way CBMC spells it ----------===//

#include "CProver.h"

#include "clang/AST/ASTContext.h"
#include "clang/AST/Expr.h"
#include "clang/AST/PrettyPrinter.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/ADT/StringSwitch.h"
#include "llvm/Support/raw_ostream.h"

using namespace clang;

namespace ccontracts {
namespace {

/// The never-defined helpers the stock-clang target lowers a predicate to, and
/// what CBMC calls the same thing. c_contracts.h declares these and nothing
/// defines them, which is what makes the mapping total: a predicate reaching
/// this tool through a diagnose_if can only name one of them.
llvm::StringRef cproverName(llvm::StringRef Helper) {
  return llvm::StringSwitch<llvm::StringRef>(Helper)
      .Case("__c_readable", "__CPROVER_r_ok")
      .Case("__c_writable", "__CPROVER_w_ok")
      .Case("__c_fresh", "__CPROVER_is_fresh")
      .Case("__c_same_object", "__CPROVER_same_object")
      .Case("__c_pointer_offset", "__CPROVER_POINTER_OFFSET")
      .Default({});
}

/// The helper a call names, or empty if the call is not one of them.
///
/// Deciding from the resolved callee rather than from the printed text is what
/// keeps a project's own function of the same name out of the mapping: the
/// header's helpers are declared and never defined, so a callee with a body is
/// somebody else's.
llvm::StringRef helperCalled(const CallExpr *Call) {
  const FunctionDecl *Callee = Call->getDirectCallee();
  if (!Callee || Callee->isDefined())
    return {};
  return cproverName(Callee->getName()).empty() ? llvm::StringRef()
                                                : Callee->getName();
}

/// Prints a predicate with the header's helpers rendered as CBMC's intrinsics.
class CProverPrinter : public PrinterHelper {
public:
  const llvm::DenseMap<const ValueDecl *, std::string> *Renamed = nullptr;
  PrintingPolicy Policy{LangOptions()};

  bool handledStmt(Stmt *S, raw_ostream &OS) override {
    if (auto *DRE = dyn_cast<DeclRefExpr>(S)) {
      if (Renamed) {
        auto It = Renamed->find(DRE->getDecl());
        if (It != Renamed->end()) {
          OS << It->second;
          return true;
        }
      }
      return false;
    }

    auto *Call = dyn_cast<CallExpr>(S);
    if (!Call)
      return false;
    llvm::StringRef Helper = helperCalled(Call);
    if (Helper.empty())
      return false;

    OS << cproverName(Helper) << "(";
    for (unsigned I = 0, N = Call->getNumArgs(); I != N; ++I) {
      if (I)
        OS << ", ";
      Call->getArg(I)->printPretty(OS, this, Policy);
    }
    OS << ")";
    return true;
  }
};

std::string
printCProver(const Expr *E, const ASTContext &Ctx,
             const llvm::DenseMap<const ValueDecl *, std::string> *Renamed) {
  std::string Text;
  llvm::raw_string_ostream OS(Text);
  CProverPrinter Helper;
  Helper.Policy = Ctx.getPrintingPolicy();
  Helper.Renamed = Renamed;
  E->printPretty(OS, &Helper, Helper.Policy);
  return Text;
}

/// Splits a predicate at its top-level `&&`.
///
/// The pieces are handled one at a time because `fresh` is an allocation rather
/// than an assumption, and `pre (fresh(p, n) && n > 0)` has to reach the entry
/// point as both.
void collectConjuncts(const Expr *E, std::vector<const Expr *> &Out) {
  const auto *BO = dyn_cast<BinaryOperator>(E->IgnoreParenImpCasts());
  if (BO && BO->getOpcode() == BO_LAnd) {
    collectConjuncts(BO->getLHS(), Out);
    collectConjuncts(BO->getRHS(), Out);
    return;
  }
  Out.push_back(E->IgnoreParenImpCasts());
}

/// The call in \p E if it is exactly a call to \p Helper.
const CallExpr *asHelperCall(const Expr *E, llvm::StringRef Helper) {
  const auto *Call = dyn_cast<CallExpr>(E->IgnoreParenImpCasts());
  if (!Call || helperCalled(Call) != Helper)
    return nullptr;
  return Call;
}

/// Every parameter \p E names.
void parmsIn(const Expr *E, llvm::SmallVectorImpl<const ParmVarDecl *> &Out) {
  struct V : RecursiveASTVisitor<V> {
    llvm::SmallVectorImpl<const ParmVarDecl *> &Out;
    explicit V(llvm::SmallVectorImpl<const ParmVarDecl *> &Out) : Out(Out) {}
    bool VisitDeclRefExpr(DeclRefExpr *DRE) {
      if (const auto *P = dyn_cast<ParmVarDecl>(DRE->getDecl()))
        Out.push_back(P);
      return true;
    }
  } Vis(Out);
  Vis.TraverseStmt(const_cast<Expr *>(E));
}

} // namespace

std::vector<MissingFresh> findWritesWithoutFresh(const Contract &C) {
  if (!C.Fn)
    return {};
  const ASTContext &Ctx = C.Fn->getASTContext();

  llvm::StringSet<> Fresh;
  std::vector<MissingFresh> Claims;

  for (const Clause &Cl : C.Clauses) {
    if (Cl.Kind != ClauseKind::Pre || !Cl.Cond)
      continue;
    std::vector<const Expr *> Conjuncts;
    collectConjuncts(Cl.Cond, Conjuncts);
    for (const Expr *Pred : Conjuncts) {
      // The header wraps every macro argument in parentheses, so the naked
      // expression is what a reader would recognise -- and it is also what
      // makes the two sides of the comparison below comparable at all.
      auto Naked = [&](const Expr *E) {
        return printCProver(E->IgnoreParenImpCasts(), Ctx, nullptr);
      };
      if (const CallExpr *F = asHelperCall(Pred, "__c_fresh")) {
        Fresh.insert(Naked(F->getArg(0)));
        continue;
      }
      for (llvm::StringRef Helper : {"__c_writable", "__c_readable"}) {
        const CallExpr *Call = asHelperCall(Pred, Helper);
        if (!Call)
          continue;
        Claims.push_back({Naked(Call->getArg(0)), Naked(Call->getArg(1)),
                          Cl.Text, Helper == "__c_writable", Cl.Loc});
      }
    }
  }

  std::vector<MissingFresh> Out;
  for (MissingFresh &M : Claims)
    if (!Fresh.contains(M.Pointer))
      Out.push_back(std::move(M));
  return Out;
}

bool emitHarness(const Contract &C, llvm::StringRef HarnessName,
                 llvm::StringRef VacuityName,
                 const llvm::StringMap<std::string> &Bounds, Harness &Out,
                 std::string &Err) {
  if (!C.Fn) {
    Err = "no contract to build an entry point from";
    return false;
  }
  const FunctionDecl *FD = C.Fn;
  const ASTContext &Ctx = FD->getASTContext();
  if (FD->isVariadic()) {
    Err = "a variadic function needs a hand-written harness";
    return false;
  }

  llvm::DenseMap<const ValueDecl *, std::string> Renamed;
  std::vector<std::string> ArgNames;
  for (unsigned I = 0, N = FD->getNumParams(); I != N; ++I) {
    std::string Name = "__contract_arg_" + std::to_string(I);
    Renamed[FD->getParamDecl(I)] = Name;
    ArgNames.push_back(std::move(Name));
  }
  auto Print = [&](const Expr *E) { return printCProver(E, Ctx, &Renamed); };

  struct Step {
    bool IsAlloc;
    std::string Text; ///< predicate, or allocation target
    std::string Size; ///< allocation only
  };
  std::vector<Step> Steps;
  llvm::StringSet<> Allocated;

  // A parameter is "bounded" once an assumption has capped it. Clauses are
  // emitted in source order, so a size read before the clause bounding it makes
  // the entry point allocate an unbounded object and everything downstream then
  // holds vacuously or fails for the wrong reason. Allocating a pointer says
  // nothing about the value stored through it, so fresh() never bounds.
  llvm::SmallPtrSet<const ParmVarDecl *, 8> Bounded;

  // --bound runs first: it exists precisely for the parameter the contract
  // leaves open, and a cap applied after the allocation that reads it would
  // bound nothing.
  for (unsigned I = 0, N = FD->getNumParams(); I != N; ++I) {
    const ParmVarDecl *P = FD->getParamDecl(I);
    auto It = Bounds.find(P->getName());
    if (It == Bounds.end())
      continue;
    Steps.push_back({false, ArgNames[I] + " <= " + It->second, ""});
    Bounded.insert(P);
  }

  auto recordBounds = [&](const Expr *E) {
    const auto *BO = dyn_cast<BinaryOperator>(E->IgnoreParenImpCasts());
    if (!BO)
      return;
    const Expr *BoundedExpr = nullptr;
    if ((BO->getOpcode() == BO_LT || BO->getOpcode() == BO_LE ||
         BO->getOpcode() == BO_EQ) &&
        BO->getRHS()->isIntegerConstantExpr(Ctx))
      BoundedExpr = BO->getLHS();
    else if ((BO->getOpcode() == BO_GT || BO->getOpcode() == BO_GE ||
              BO->getOpcode() == BO_EQ) &&
             BO->getLHS()->isIntegerConstantExpr(Ctx))
      BoundedExpr = BO->getRHS();
    if (!BoundedExpr)
      return;
    llvm::SmallVector<const ParmVarDecl *, 4> Params;
    parmsIn(BoundedExpr, Params);
    Bounded.insert(Params.begin(), Params.end());
  };

  for (const Clause &Cl : C.Clauses) {
    if (Cl.Kind != ClauseKind::Pre || !Cl.Cond)
      continue;
    std::vector<const Expr *> Conjuncts;
    collectConjuncts(Cl.Cond, Conjuncts);
    for (const Expr *Pred : Conjuncts) {
      const CallExpr *Call = asHelperCall(Pred, "__c_fresh");
      const Expr *Target =
          Call ? Call->getArg(0)->IgnoreParenImpCasts() : nullptr;
      if (Target && Target->isLValue()) {
        const Expr *Size = Call->getArg(1);
        llvm::SmallVector<const ParmVarDecl *, 4> Used;
        parmsIn(Size, Used);
        for (const ParmVarDecl *P : Used) {
          if (Bounded.count(P))
            continue;
          Out.Warnings.push_back(
              ("the object allocated for " +
               printCProver(Target, Ctx, nullptr) +
               " is unbounded: nothing has capped '" + P->getName() +
               "' yet. Put a bound on it before the fresh clause, or pass "
               "--bound " +
               P->getName() + "=N")
                  .str());
          break;
        }
        if (!Size->isIntegerConstantExpr(Ctx))
          Out.SymbolicExtents = true;
        std::string TargetText = Print(Target);
        if (Allocated.insert(TargetText).second)
          Steps.push_back({true, std::move(TargetText), Print(Size)});
        continue;
      }
      recordBounds(Pred);
      Steps.push_back({false, Print(Pred), ""});
    }
  }

  auto emitPreamble = [&](llvm::raw_ostream &OS) {
    for (unsigned I = 0, N = FD->getNumParams(); I != N; ++I) {
      OS << "  ";
      FD->getParamDecl(I)->getType().print(OS, Ctx.getPrintingPolicy(),
                                           ArgNames[I]);
      OS << ";\n";
    }
    for (const Step &St : Steps) {
      if (St.IsAlloc)
        OS << "  " << St.Text << " = __CPROVER_allocate(" << St.Size
           << ", 0);\n";
      else
        OS << "  __CPROVER_assume(" << St.Text << ");\n";
    }
  };

  {
    llvm::raw_string_ostream OS(Out.Text);
    OS << "\n/* entry point generated from " << FD->getName()
       << "'s contract; do not write one by hand */\n";
    OS << "void " << HarnessName << "(void) {\n";
    emitPreamble(OS);
    OS << "  " << FD->getName() << "(";
    for (unsigned I = 0, N = ArgNames.size(); I != N; ++I)
      OS << (I ? ", " : "") << ArgNames[I];
    OS << ");\n}\n";
  }

  // The probe stops where the harness starts calling. Asserting false after the
  // call would also require the call to return, so a function that cannot
  // terminate under its own contract would be reported as vacuous -- a wrong
  // answer to a question about the preconditions alone. Stopping short asks
  // only what was meant: can anything satisfy them?
  {
    llvm::raw_string_ostream OS(Out.VacuityText);
    OS << "\n/* the same assumptions, asserting false: reachable unless they "
          "are unsatisfiable */\n";
    OS << "void " << VacuityName << "(void) {\n";
    emitPreamble(OS);
    OS << "  __CPROVER_assert(0, \"" << FD->getName()
       << "'s preconditions are satisfiable\");\n}\n";
  }
  return true;
}

std::vector<std::string> extractClauses(llvm::StringRef Text, bool Canonical) {
  static constexpr llvm::StringLiteral Kinds[] = {
      "__CPROVER_requires",        "__CPROVER_ensures",
      "__CPROVER_assigns",         "__CPROVER_loop_invariant",
      "__CPROVER_decreases",       "__CPROVER_requires_contract",
      "__CPROVER_ensures_contract"};

  std::vector<std::string> Out;
  for (size_t I = 0; I < Text.size();) {
    llvm::StringRef Rest = Text.substr(I);
    llvm::StringRef Kind;
    for (llvm::StringRef K : Kinds)
      if (Rest.starts_with(K) && (Kind.empty() || K.size() > Kind.size()))
        Kind = K;
    // A longer identifier that merely contains a clause keyword is not one.
    if (Kind.empty() || (I > 0 && (isalnum((unsigned char)Text[I - 1]) ||
                                   Text[I - 1] == '_'))) {
      ++I;
      continue;
    }
    // A clause is a call. The bare name is something else's identifier.
    size_t P = I + Kind.size();
    while (P < Text.size() && isspace((unsigned char)Text[P]))
      ++P;
    if (P >= Text.size() || Text[P] != '(') {
      I += Kind.size();
      continue;
    }

    std::string Canon = Kind.str();
    int Depth = 0;
    size_t Q = P;
    for (; Q < Text.size(); ++Q) {
      char Ch = Text[Q];
      if (Ch == '(')
        ++Depth;
      else if (Ch == ')') {
        if (--Depth == 0) {
          if (!Canonical)
            Canon += Ch;
          ++Q;
          break;
        }
      }
      // Parentheses go with the whitespace, and for the same reason: the two
      // renderings of one clause differ in how much they parenthesise and in
      // nothing else. What survives -- the intrinsic names, the operators, the
      // operand order -- is what a drift would change.
      if (Canonical) {
        if (Ch != '(' && Ch != ')' && !isspace((unsigned char)Ch))
          Canon += Ch;
      } else if (isspace((unsigned char)Ch)) {
        // A clause spans lines when the author wrote it that way; displaying it
        // is a one-line job.
        if (!Canon.empty() && Canon.back() != ' ')
          Canon += ' ';
      } else
        Canon += Ch;
    }
    Out.push_back(std::move(Canon));
    I = Q;
  }
  return Out;
}

} // namespace ccontracts
