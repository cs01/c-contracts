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

#include "CProver.h"
#include "CallSite.h"
#include "Contract.h"
#include "Prove.h"

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
#include "llvm/Support/MemoryBuffer.h"
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

//===----------------------------------------------------------------------===//
// prove
//===----------------------------------------------------------------------===//

llvm::cl::opt<std::string>
    Caller("caller",
           llvm::cl::desc("Verify this caller against the proved function's "
                          "contract, where a precondition is an obligation"),
           llvm::cl::cat(Category));

llvm::cl::opt<std::string>
    Mode("mode", llvm::cl::desc("auto (default), enforce, or harness"),
         llvm::cl::init("auto"), llvm::cl::cat(Category));

llvm::cl::opt<bool>
    NoVacuity("no-vacuity",
              llvm::cl::desc("Skip the check that the preconditions are "
                             "satisfiable at all"),
              llvm::cl::cat(Category));

llvm::cl::opt<bool> Verbose("verbose",
                            llvm::cl::desc("Print what CBMC printed"),
                            llvm::cl::cat(Category));

llvm::cl::opt<unsigned> Timeout("timeout",
                                llvm::cl::desc("Seconds any one step may take"),
                                llvm::cl::init(900), llvm::cl::cat(Category));

llvm::cl::opt<std::string>
    Solver("solver",
           llvm::cl::desc("sat, or an installed SMT solver; default is chosen "
                          "from the shape of the harness"),
           llvm::cl::cat(Category));

llvm::cl::opt<std::string>
    ProofDir("proof-dir",
             llvm::cl::desc("Where <function>.proof.c may override the "
                            "generated entry point"),
             llvm::cl::init("proofs"), llvm::cl::cat(Category));

llvm::cl::opt<std::string>
    PreprocessorCC("cc",
                   llvm::cl::desc("The preprocessor that lowers the clauses"),
                   llvm::cl::init("cc"), llvm::cl::cat(Category));

llvm::cl::list<std::string>
    Bounds("bound",
           llvm::cl::desc("name=N, for a size the contract leaves "
                          "open"),
           llvm::cl::cat(Category));

llvm::cl::opt<unsigned>
    Unwind("unwind",
           llvm::cl::desc("CBMC's unwind bound, with unwinding assertions on"),
           llvm::cl::cat(Category));

llvm::cl::list<std::string> CBMCFlags("cbmc-flag",
                                      llvm::cl::desc("Passed through to cbmc"),
                                      llvm::cl::cat(Category));

llvm::cl::opt<bool> KeepWork("keep-work",
                             llvm::cl::desc("Keep the working directory"),
                             llvm::cl::cat(Category));

/// Set from argv before the option parser runs; empty unless the `prove`
/// subcommand was asked for.
std::string ProveFunction;

/// What prove exited with, since it runs inside the AST consumer.
int ProveStatus = 0;

/// Clang's own headers, from the LLVM this was built against. Every parse the
/// tool performs needs them, and it cannot derive them from argv[0] the way the
/// driver does, because it does not live in an LLVM install.
std::string resourceDirArg(const std::vector<std::string> &Existing) {
  for (const std::string &A : Existing)
    if (llvm::StringRef(A).starts_with("-resource-dir"))
      return {};
  return std::string("-resource-dir=") + C_CONTRACTS_RESOURCE_DIR;
}

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

    if (!ProveFunction.empty()) {
      ProveStatus = prove(Contracts, Ctx);
      return;
    }

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
  /// Level 3. The contract is already type-checked by the time this runs; what
  /// is left is whether it is true for every input, which is CBMC's job.
  int prove(const std::vector<Contract> &Contracts, ASTContext &Ctx) {
    ProveOptions Opts;
    Opts.Function = ProveFunction;
    Opts.Caller = Caller;
    Opts.Mode = Mode;
    Opts.Vacuity = !NoVacuity;
    Opts.Verbose = Verbose;
    Opts.Timeout = Timeout;
    Opts.Solver = Solver;
    Opts.ProofDir = ProofDir;
    Opts.CC = PreprocessorCC;
    Opts.KeepWork = KeepWork;
    Opts.Unwind = Unwind;
    for (const std::string &F : CBMCFlags)
      Opts.CBMCFlags.push_back(F);
    for (const std::string &B : Bounds) {
      auto [Name, Value] = llvm::StringRef(B).split('=');
      if (Value.empty()) {
        llvm::errs() << "error: --bound wants name=N, not '" << B << "'\n";
        return 2;
      }
      Opts.Bounds[Name] = Value.str();
    }

    Contract Wanted;
    for (const Contract &C : Contracts)
      if (C.Fn->getName() == ProveFunction)
        Wanted = C;

    const SourceManager &SM = Ctx.getSourceManager();
    llvm::StringRef File;
    if (auto Main = SM.getFileEntryRefForID(SM.getMainFileID()))
      File = Main->getName();
    return runProve(Wanted, File, Args, Opts,
                    Ctx.getDiagnostics().hasErrorOccurred());
  }

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
    if (std::string R = resourceDirArg(Out); !R.empty())
      Out.push_back(std::move(R));
    return Out;
  }

  const tooling::CompilationDatabase &DB;
  std::vector<std::string> Current;
};

} // namespace

/// Prints every contract clause a file carries in CBMC's spelling, canonically.
///
/// This is what both halves of the differential gate go through, so the gate
/// cannot be passed by comparing two texts with two different canonicalisers.
int printClauses(int argc, const char **argv) {
  for (int I = 2; I < argc; ++I) {
    auto Buf = llvm::MemoryBuffer::getFile(argv[I]);
    if (!Buf) {
      llvm::errs() << "error: cannot read " << argv[I] << "\n";
      return 2;
    }
    for (const std::string &C :
         extractClauses((*Buf)->getBuffer(), /*Canonical=*/true))
      llvm::outs() << C << "\n";
  }
  return 0;
}

int main(int argc, const char **argv) {
  // Two subcommands sit in front of the option parser, because both take a
  // positional argument that is not a source file and llvm::cl has nowhere to
  // put one.
  std::vector<const char *> Argv(argv, argv + argc);
  if (argc > 1 && llvm::StringRef(argv[1]) == "clauses")
    return printClauses(argc, argv);
  if (argc > 2 && llvm::StringRef(argv[1]) == "prove") {
    ProveFunction = argv[2];
    Argv.erase(Argv.begin() + 1, Argv.begin() + 3);
  } else if (argc > 1 && llvm::StringRef(argv[1]) == "check") {
    Argv.erase(Argv.begin() + 1);
  }
  argc = static_cast<int>(Argv.size());
  argv = Argv.data();

  auto Parser = tooling::CommonOptionsParser::create(argc, argv, Category);
  if (!Parser) {
    llvm::errs() << toString(Parser.takeError());
    return 2;
  }

  tooling::ClangTool Tool(Parser->getCompilations(),
                          Parser->getSourcePathList());

  IgnoringDiagConsumer Ignore;
  Tool.setDiagnosticConsumer(&Ignore);
  Tool.appendArgumentsAdjuster(tooling::getInsertArgumentAdjuster(
      resourceDirArg({}).c_str(), tooling::ArgumentInsertPosition::BEGIN));
  CheckActionFactory Factory(Parser->getCompilations());
  if (Tool.run(&Factory) != 0)
    return 2;

  if (!ProveFunction.empty())
    return ProveStatus;

  if (SawError)
    return 1;
  if (SawWarning && WarningsAsErrors)
    return 1;
  return 0;
}
