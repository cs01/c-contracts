//===- Ghost.cpp - Type-check a clause in a scope built for it ------------===//
//
/// \file
/// Stock clang parses a precondition because diagnose_if hands it the
/// function's prototype scope. Nothing hands it that scope for a
/// postcondition, so this file builds one: a function with the same parameter
/// list, plus a local bound to the return type, whose body is the clause.
///
/// The translation unit is reparsed with those functions appended, so the
/// clause is checked with every typedef, macro and declaration it was written
/// against still in force -- which is the only context where it means what the
/// author meant. Diagnostics come back as clang's own, reported against the
/// annotation in the user's source.
//
//===----------------------------------------------------------------------===//

#include "Contract.h"

#include "clang/AST/ASTContext.h"
#include "clang/Basic/Diagnostic.h"
#include "clang/Basic/FileManager.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Frontend/FrontendActions.h"
#include "clang/Lex/Lexer.h"
#include "clang/Tooling/Tooling.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/VirtualFileSystem.h"
#include "llvm/Support/raw_ostream.h"

using namespace clang;

namespace ccontracts {

namespace {

/// Warnings that only ever fire because of how a ghost is shaped, never because
/// of anything the user wrote: the function is unused, its parameters are
/// unused, and the result binding is deliberately never assigned.
const char *const GhostNoiseFlags[] = {
    "-Wno-unused-function",    "-Wno-unused-parameter",
    "-Wno-unused-variable",    "-Wno-unused-but-set-variable",
    "-Wno-uninitialized",      "-Wno-sometimes-uninitialized",
    "-Wno-missing-prototypes",
};

/// CompilerInstance::ExecuteAction prints its own "N errors generated" summary
/// straight to stderr whenever carets are on. Nothing in the ghost pass should
/// reach the terminal on its own -- every diagnostic worth showing comes back
/// through the consumer and is reported against the user's own source.
constexpr llvm::StringLiteral SilenceSummary = "-fno-caret-diagnostics";

constexpr llvm::StringLiteral BodyPrefix = "  return (";

/// A clause needs somewhere to be checked, and that means the declaration it
/// came from has to name its parameters. A prototype written `void f(int *,
/// size_t)` names none, so there is nothing to call them by.
const FunctionDecl *declWithNamedParams(const FunctionDecl *FD) {
  for (const FunctionDecl *R : FD->redecls()) {
    bool AllNamed = true;
    for (const ParmVarDecl *P : R->parameters())
      if (P->getName().empty()) {
        AllNamed = false;
        break;
      }
    if (AllNamed)
      return R;
  }
  return nullptr;
}

std::string parameterListText(const FunctionDecl *FD) {
  if (FD->getNumParams() == 0)
    return FD->isVariadic() ? "" : "void";

  const ASTContext &Ctx = FD->getASTContext();
  SourceRange R = FD->getParametersSourceRange();
  llvm::StringRef Text =
      Lexer::getSourceText(CharSourceRange::getTokenRange(R),
                           Ctx.getSourceManager(), Ctx.getLangOpts());
  return Text.str();
}

/// `__typeof__(f(a, b))` is the return type of f, spelled without naming it --
/// which matters because a C function cannot name its own return type, and the
/// type may be a local typedef with no other spelling. Legal all the way back
/// to C89 as a GNU extension, and unevaluated, so nothing is called.
/// Empty for a function returning void: there is no result to bind, and a
/// clause that names one gets clang's own "use of undeclared identifier
/// 'contract_result'" pointing at the clause, which is the right thing to say. A void
/// function can still carry a postcondition about what it wrote through a
/// pointer, and that one has to keep working.
std::string resultBinding(const FunctionDecl *FD) {
  if (FD->getReturnType()->isVoidType())
    return "";

  std::string Call = FD->getNameAsString() + "(";
  for (unsigned I = 0, N = FD->getNumParams(); I != N; ++I) {
    if (I)
      Call += ", ";
    Call += FD->getParamDecl(I)->getNameAsString();
  }
  Call += ")";
  return "  __typeof__(" + Call + ") contract_result;\n";
}

struct GhostSource {
  std::string Text;
  /// First line of each ghost (1-based) -> the clause it was built for. Keyed
  /// on the first line, not the predicate line, because a diagnostic can land
  /// on the signature or the result binding above it.
  llvm::DenseMap<unsigned, unsigned> LineToClause;
  std::vector<const Clause *> Clauses;
  /// Clauses that could not be given a scope at all, with the reason.
  std::vector<GhostDiagnostic> Rejected;
};

/// Appends a ghost per checkable clause to \p Original.
GhostSource buildGhostSource(llvm::StringRef Original,
                             const std::vector<Contract> &Contracts) {
  GhostSource G;
  llvm::raw_string_ostream OS(G.Text);
  OS << Original;
  if (!Original.ends_with("\n"))
    OS << "\n";

  unsigned Line = Original.count('\n') + (Original.ends_with("\n") ? 1 : 2);

  auto emit = [&](llvm::StringRef S) {
    OS << S;
    Line += S.count('\n');
  };

  emit("\n/* Appended by c-contracts. Each function below exists so that one\n"
       "   clause can be name-resolved and type-checked in the scope it was\n"
       "   written against. Nothing here is compiled into the program. */\n");

  // In a scope whose parameters are the entry values, `old(x)` is x. The
  // header leaves the clause quoted, so this is the first time the spelling is
  // expanded, and it is expanded to the right thing for this scope.
  emit("#undef contract_old\n#define contract_old(E) (E)\n#undef old\n#define old(E) (E)\n");

  unsigned N = 0;
  for (const Contract &C : Contracts) {
    if (!C.needsGhost())
      continue;

    const FunctionDecl *Scope = declWithNamedParams(C.Fn);
    if (!Scope) {
      for (const Clause &Cl : C.Clauses)
        if (Cl.Kind != ClauseKind::Pre)
          G.Rejected.push_back(
              {Cl.Loc, true,
               ("cannot check this " + describe(Cl.Kind) +
                ": no declaration of '" + C.Fn->getNameAsString() +
                "' names its parameters")
                   .str()});
      continue;
    }

    for (const Clause &Cl : C.Clauses) {
      if (Cl.Kind == ClauseKind::Pre)
        continue; // stock clang already checked it.

      G.LineToClause[Line] = G.Clauses.size();
      G.Clauses.push_back(&Cl);

      std::string Name = "__c_ghost_" + std::to_string(N++);
      emit("static int " + Name + "(" + parameterListText(Scope) + ") {\n");
      emit(resultBinding(Scope));
      emit(BodyPrefix.str() + Cl.Text + ");\n");
      emit("}\n");
    }
  }

  emit("#undef contract_old\n#undef old\n");
  return G;
}

/// Keeps the diagnostics that belong to a ghost body, and drops everything
/// else: the original translation unit is in this file too, and every
/// diagnostic it produces was already reported by the first pass.
class GhostDiagnosticConsumer : public DiagnosticConsumer {
public:
  GhostDiagnosticConsumer(const GhostSource &G, unsigned FirstGhostLine)
      : G(G), FirstGhostLine(FirstGhostLine) {}

  void HandleDiagnostic(DiagnosticsEngine::Level Level,
                        const Diagnostic &Info) override {
    DiagnosticConsumer::HandleDiagnostic(Level, Info);

    if (Level < DiagnosticsEngine::Warning || !Info.hasSourceManager())
      return;
    const SourceManager &SM = Info.getSourceManager();
    SourceLocation Loc = Info.getLocation();
    if (Loc.isInvalid())
      return;
    Loc = SM.getExpansionLoc(Loc);
    if (SM.getFileID(Loc) != SM.getMainFileID())
      return;

    unsigned L = SM.getExpansionLineNumber(Loc);
    if (L < FirstGhostLine)
      return;

    // The nearest ghost start at or above the diagnostic is the one it belongs
    // to, wherever inside that ghost it landed.
    const Clause *Cl = nullptr;
    for (unsigned Probe = L; Probe >= FirstGhostLine; --Probe)
      if (auto It = G.LineToClause.find(Probe); It != G.LineToClause.end()) {
        Cl = G.Clauses[It->second];
        break;
      }
    if (!Cl)
      return;

    llvm::SmallString<256> Msg;
    Info.FormatDiagnostic(Msg);
    Out.push_back(
        {Cl->Loc, Level >= DiagnosticsEngine::Error, Msg.str().str()});
  }

  std::vector<GhostDiagnostic> take() { return std::move(Out); }

private:
  const GhostSource &G;
  unsigned FirstGhostLine;
  std::vector<GhostDiagnostic> Out;
};

} // namespace

std::vector<GhostDiagnostic>
checkClausesInGhostScope(const std::vector<Contract> &Contracts,
                         ASTContext &Ctx,
                         const std::vector<std::string> &Args) {
  const SourceManager &SM = Ctx.getSourceManager();
  FileID Main = SM.getMainFileID();
  llvm::StringRef Original = SM.getBufferData(Main);

  GhostSource G = buildGhostSource(Original, Contracts);
  std::vector<GhostDiagnostic> Result = G.Rejected;
  if (G.Clauses.empty())
    return Result;

  const unsigned FirstGhostLine = Original.count('\n') + 1;

  // The ghost file has to sit where the original did, or a #include "..." in it
  // resolves against the wrong directory.
  llvm::SmallString<256> GhostPath(SM.getFileEntryRefForID(Main)->getName());
  llvm::sys::path::remove_filename(GhostPath);
  llvm::sys::path::append(GhostPath, "__c_contracts_ghost__.c");

  auto InMemFS = llvm::makeIntrusiveRefCnt<llvm::vfs::InMemoryFileSystem>();
  InMemFS->addFile(GhostPath, 0, llvm::MemoryBuffer::getMemBufferCopy(G.Text));
  auto OverlayFS = llvm::makeIntrusiveRefCnt<llvm::vfs::OverlayFileSystem>(
      llvm::vfs::getRealFileSystem());
  OverlayFS->pushOverlay(InMemFS);
  auto Files =
      llvm::makeIntrusiveRefCnt<FileManager>(FileSystemOptions(), OverlayFS);

  std::vector<std::string> CmdLine;
  CmdLine.push_back("c-contracts");
  for (const std::string &A : Args)
    CmdLine.push_back(A);
  CmdLine.push_back("-fsyntax-only");
  CmdLine.push_back(SilenceSummary.str());
  for (const char *F : GhostNoiseFlags)
    CmdLine.push_back(F);
  CmdLine.push_back(std::string(GhostPath));

  GhostDiagnosticConsumer Consumer(G, FirstGhostLine);
  tooling::ToolInvocation Inv(CmdLine, std::make_unique<SyntaxOnlyAction>(),
                              Files.get());
  Inv.setDiagnosticConsumer(&Consumer);
  Inv.run();

  for (GhostDiagnostic &D : Consumer.take())
    Result.push_back(std::move(D));
  return Result;
}

} // namespace ccontracts
