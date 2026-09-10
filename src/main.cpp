//===- main.cpp - c-contracts driver --------------------------------------===//
//
/// \file
/// `c-contracts check` reads the contracts an ordinary clang left in the AST
/// and reports what they get wrong.
///
/// The compiler is not patched and not forked: everything here runs against a
/// released clang, and everything it checks arrived through c_contracts.h.
//
//===----------------------------------------------------------------------===//

#include "CallSite.h"
#include "Contract.h"

#include "clang/AST/ASTConsumer.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/Analysis/AnalysisDeclContext.h"
#include "clang/Basic/Diagnostic.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/FrontendAction.h"
#include "clang/Tooling/CommonOptionsParser.h"
#include "clang/Tooling/Tooling.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/raw_ostream.h"

using namespace clang;
using namespace ccontracts;

namespace {

llvm::cl::OptionCategory Category("c-contracts options");

llvm::cl::opt<bool>
    ListClauses("list", llvm::cl::desc("Print every contract clause found"),
                llvm::cl::cat(Category));

llvm::cl::opt<bool> WarningsAsErrors(
    "warnings-as-errors",
    llvm::cl::desc("Exit non-zero if any contract problem is reported"),
    llvm::cl::cat(Category));

/// Set by whatever reports a problem; read by main for the exit status.
bool SawError = false;
bool SawWarning = false;

void report(const SourceManager &SM, SourceLocation Loc, bool IsError,
            llvm::StringRef Message) {
  (IsError ? SawError : SawWarning) = true;

  llvm::raw_ostream &OS = llvm::errs();
  if (Loc.isValid())
    OS << Loc.printToString(SM) << ": ";
  OS << (IsError ? "error: " : "warning: ") << Message << "\n";
}

class ContractCollector : public RecursiveASTVisitor<ContractCollector> {
public:
  explicit ContractCollector(std::vector<Contract> &Out) : Out(Out) {}

  bool VisitFunctionDecl(FunctionDecl *FD) {
    // A contract is inherited onto every redeclaration, so collecting from all
    // of them would report each clause once per declaration.
    if (FD != FD->getCanonicalDecl())
      return true;
    Contract C = collectContract(FD);
    if (!C.empty())
      Out.push_back(std::move(C));
    return true;
  }

private:
  std::vector<Contract> &Out;
};

/// Every function this translation unit has a body for: the bodies the
/// call-site pass can build a CFG over.
class BodyCollector : public RecursiveASTVisitor<BodyCollector> {
public:
  explicit BodyCollector(std::vector<const FunctionDecl *> &Out) : Out(Out) {}

  bool VisitFunctionDecl(FunctionDecl *FD) {
    if (FD->hasBody() && FD->isThisDeclarationADefinition())
      Out.push_back(FD);
    return true;
  }

private:
  std::vector<const FunctionDecl *> &Out;
};

/// Turns what the call-site pass finds into the tool's own diagnostics.
///
/// The wording is the contract-aware front end's, verbatim, so the two
/// implementations can be diffed against each other on the same source.
class Reporter : public ContractViolationReporter {
public:
  explicit Reporter(const SourceManager &SM) : SM(SM) {}

  void reportPreconditionViolated(const CallExpr *Call,
                                  const FunctionDecl *Callee,
                                  const Clause &C) override {
    report(SM, Call->getBeginLoc(), /*IsError=*/false,
           "precondition " + C.Text + " of '" + Callee->getName().str() +
               "' is violated by this call");
  }

  void reportPreconditionNotGuaranteed(const CallExpr *Call,
                                       const FunctionDecl *Callee,
                                       const Clause &C) override {
    report(SM, Call->getBeginLoc(), /*IsError=*/false,
           "precondition " + C.Text + " of '" + Callee->getName().str() +
               "' is not guaranteed by the constraints at this call");
  }

private:
  const SourceManager &SM;
};

class CheckConsumer : public ASTConsumer {
public:
  explicit CheckConsumer(std::vector<std::string> Args)
      : Args(std::move(Args)) {}

  void HandleTranslationUnit(ASTContext &Ctx) override {
    std::vector<Contract> Contracts;
    ContractCollector(Contracts).TraverseDecl(Ctx.getTranslationUnitDecl());

    const SourceManager &SM = Ctx.getSourceManager();

    if (ListClauses)
      for (const Contract &C : Contracts)
        for (const Clause &Cl : C.Clauses)
          llvm::outs() << Cl.Loc.printToString(SM) << ": " << describe(Cl.Kind)
                       << " of '" << C.Fn->getName() << "': " << Cl.Text
                       << "\n";

    for (const GhostDiagnostic &D :
         checkClausesInGhostScope(Contracts, Ctx, Args))
      report(SM, D.Loc, D.IsError, D.Message);

    // Clang's own diagnose_if only fires when the condition folds against the
    // argument expressions, so a violation that travels through a variable
    // reaches nobody. That is what this pass is here for.
    std::vector<const FunctionDecl *> Bodies;
    BodyCollector(Bodies).TraverseDecl(Ctx.getTranslationUnitDecl());

    AnalysisDeclContextManager Mgr(Ctx);
    Reporter R(SM);
    for (const FunctionDecl *FD : Bodies)
      if (AnalysisDeclContext *AC = Mgr.getContext(FD))
        runCallSiteChecking(*AC, R);
  }

private:
  std::vector<std::string> Args;
};

class CheckAction : public ASTFrontendAction {
public:
  explicit CheckAction(std::vector<std::string> Args) : Args(std::move(Args)) {}

  std::unique_ptr<ASTConsumer> CreateASTConsumer(CompilerInstance &,
                                                 llvm::StringRef) override {
    return std::make_unique<CheckConsumer>(Args);
  }

private:
  std::vector<std::string> Args;
};

/// Hands each file's own compile command to the action, which needs it to
/// reparse the same translation unit for the ghost pass.
class CheckActionFactory : public tooling::FrontendActionFactory {
public:
  explicit CheckActionFactory(const tooling::CompilationDatabase &DB)
      : DB(DB) {}

  std::unique_ptr<FrontendAction> create() override {
    return std::make_unique<CheckAction>(Current);
  }

  bool runInvocation(std::shared_ptr<CompilerInvocation> Invocation,
                     FileManager *Files,
                     std::shared_ptr<PCHContainerOperations> PCHContainerOps,
                     DiagnosticConsumer *DiagConsumer) override {
    Current = argsFor(Invocation->getFrontendOpts().Inputs.empty()
                          ? ""
                          : Invocation->getFrontendOpts().Inputs[0].getFile());
    return tooling::FrontendActionFactory::runInvocation(
        std::move(Invocation), Files, std::move(PCHContainerOps), DiagConsumer);
  }

private:
  /// The command minus argv[0] and minus the input file: what the ghost pass
  /// has to reproduce so the second parse sees the same translation unit.
  std::vector<std::string> argsFor(llvm::StringRef File) const {
    std::vector<std::string> Out;
    auto Commands = DB.getCompileCommands(File);
    if (Commands.empty())
      return Out;
    const std::vector<std::string> &Line = Commands.front().CommandLine;
    for (size_t I = 1; I < Line.size(); ++I) {
      if (Line[I] == File || llvm::StringRef(Line[I]).ends_with(File))
        continue;
      if (Line[I] == "-o" && I + 1 < Line.size()) {
        ++I;
        continue;
      }
      if (Line[I] == "-c")
        continue;
      Out.push_back(Line[I]);
    }
    return Out;
  }

  const tooling::CompilationDatabase &DB;
  std::vector<std::string> Current;
};

} // namespace

int main(int argc, const char **argv) {
  auto Parser = tooling::CommonOptionsParser::create(argc, argv, Category);
  if (!Parser) {
    llvm::errs() << toString(Parser.takeError());
    return 2;
  }

  tooling::ClangTool Tool(Parser->getCompilations(),
                          Parser->getSourcePathList());

  IgnoringDiagConsumer Ignore;
  Tool.setDiagnosticConsumer(&Ignore);
  CheckActionFactory Factory(Parser->getCompilations());
  if (Tool.run(&Factory) != 0)
    return 2;

  if (SawError)
    return 1;
  if (SawWarning && WarningsAsErrors)
    return 1;
  return 0;
}
