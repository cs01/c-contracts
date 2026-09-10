//===- Contract.h - Contract clauses recovered from a stock clang AST -----===//
//
/// \file
/// The contract model, and how it is read back out of an ordinary clang AST.
///
/// c_contracts.h leaves two kinds of trace on a declaration. A precondition
/// becomes a diagnose_if attribute, which stock clang has already parsed,
/// name-resolved and type-checked in the function's own prototype scope. Every
/// other clause becomes an annotate string, which clang only lexed -- giving
/// this tool the clause verbatim, to type-check later in a scope it builds.
//
//===----------------------------------------------------------------------===//

#ifndef C_CONTRACTS_CONTRACT_H
#define C_CONTRACTS_CONTRACT_H

#include "clang/AST/Decl.h"
#include "clang/Lex/Preprocessor.h"
#include "llvm/ADT/StringRef.h"

#include <string>
#include <vector>

namespace ccontracts {

/// The c_contracts.h this binary was built to read.
///
/// The header is meant to be vendored, so the copy in front of the tool is the
/// project's, not this repo's, and it can be years old. Bumped in lockstep with
/// C_CONTRACTS_VERSION in the header whenever a consumer could notice the
/// difference.
constexpr unsigned RequiredHeaderVersion = 1;

/// The C_CONTRACTS_VERSION \p PP saw, or 0 if the translation unit defined
/// none -- which means either a header older than versioning, or no header.
unsigned headerVersion(const clang::Preprocessor &PP);

enum class ClauseKind {
  Pre,     ///< pre(P), and the caller's half of reads/writes.
  Post,    ///< post(P).
  Returns, ///< returns(P), which is post with the result already bound.
};

/// What a diagnostic calls this clause.
llvm::StringRef describe(ClauseKind K);

/// The marker prefix c_contracts.h writes for \p K, or empty for Pre, which
/// does not travel as a marker.
llvm::StringRef markerPrefix(ClauseKind K);

struct Clause {
  ClauseKind Kind;

  /// The predicate as the user spelled it. For a marker this is the text the
  /// header quoted, before any project macro inside it expanded: that macro
  /// means the right thing only in the translation unit it came from, which is
  /// exactly where the ghost pass puts it back.
  std::string Text;

  /// The typed predicate, for a Pre. This is the contract, not the diagnose_if
  /// condition -- the negation the header wrote has been stripped. Null for a
  /// marker until the ghost pass has typed it.
  const clang::Expr *Cond = nullptr;

  /// Where a diagnostic should point in the user's source.
  clang::SourceLocation Loc;
};

struct Contract {
  const clang::FunctionDecl *Fn = nullptr;
  std::vector<Clause> Clauses;

  bool empty() const { return Clauses.empty(); }

  /// True if any clause has to be checked by building a scope for it, which is
  /// what makes a function worth a ghost.
  bool needsGhost() const;
};

/// Reads the clauses c_contracts.h left on \p FD. Returns an empty contract for
/// a function that carries none.
Contract collectContract(const clang::FunctionDecl *FD);

//===----------------------------------------------------------------------===//
// Ghost checking
//===----------------------------------------------------------------------===//

/// One diagnostic the ghost pass produced, already mapped back to the clause it
/// came from in the user's source.
struct GhostDiagnostic {
  clang::SourceLocation Loc; ///< In the *original* translation unit.
  bool IsError;
  std::string Message;
};

/// Type-checks the clauses that stock clang only lexed.
///
/// Synthesizes, for each contract in \p Contracts, a function with the same
/// parameter list and a local bound to the return type, whose body is the
/// clause. Reparses the translation unit with those appended, and reports what
/// the clauses themselves got wrong -- an undeclared name, a type mismatch, a
/// predicate that is not a condition.
///
/// \p Args is the compile command for the original file, minus argv[0].
std::vector<GhostDiagnostic>
checkClausesInGhostScope(const std::vector<Contract> &Contracts,
                         clang::ASTContext &Ctx,
                         const std::vector<std::string> &Args);

} // namespace ccontracts

#endif // C_CONTRACTS_CONTRACT_H
