//===- Prove.h - The CBMC tier
//---------------------------------------------===//
//
/// \file
/// `c-contracts prove` -- level 3, where a contract stops being type-checked
/// and starts being proved for every input.
///
/// The pipeline is the one proofs/verify-contract.sh in the fork already used,
/// with the fork taken out of it: preprocessing the same annotated source with
/// -DC_CONTRACTS_CPROVER is what lowers the clauses, so nothing here needs a
/// compiler that understands contracts. What this adds is the entry point, the
/// diagnostics for the shapes that cannot discharge, and a vacuity gate.
//
//===----------------------------------------------------------------------===//

#ifndef C_CONTRACTS_PROVE_H
#define C_CONTRACTS_PROVE_H

#include "Contract.h"

#include "llvm/ADT/StringMap.h"

#include <string>
#include <vector>

namespace ccontracts {

struct ProveOptions {
  std::string Function;

  /// Verify \p Caller against \p Function's contract instead of verifying
  /// \p Function against it. A precondition is an assumption in the first mode
  /// and an obligation in the second, and most preconditions are only ever
  /// checked in the second.
  std::string Caller;

  /// auto: check the frame, and fall back to the generated entry point when
  /// goto-instrument refuses because a loop in the function carries no
  /// contract. enforce and harness pin one of the two.
  std::string Mode = "auto";

  bool Vacuity = true;
  bool Verbose = false;
  unsigned Timeout = 900;

  /// sat, or the name of an installed SMT solver. Empty picks from the shape of
  /// the harness; see COST.md in the fork.
  std::string Solver;

  /// Where a hand-written entry point may override the generated one.
  std::string ProofDir = "proofs";

  /// The preprocessor to lower the clauses with. Not clang's -cc1: goto-cc
  /// cannot parse what clang's own headers leave behind.
  std::string CC = "cc";

  /// --bound n=64, for the size parameter a contract leaves open.
  llvm::StringMap<std::string> Bounds;

  /// CBMC's unwind bound, for the loops the generated entry point has to run
  /// rather than replace with a contract. Zero leaves it out.
  unsigned Unwind = 0;

  std::vector<std::string> CBMCFlags;

  /// Keep the working directory and say where it is.
  bool KeepWork = false;
};

/// Runs the proof. \p Args is the compile command for \p File, minus argv[0].
///
/// \p Unparsed says the translation unit had errors of its own, which is the
/// difference between "this function carries no contract" and "nothing here
/// parsed" -- two very different things to tell someone.
///
/// Returns 0 when the function verified, 1 when it did not (or when the proof
/// was vacuous), 2 when the pipeline could not be set up.
int runProve(const Contract &C, llvm::StringRef File,
             const std::vector<std::string> &Args, const ProveOptions &Opts,
             bool Unparsed);

} // namespace ccontracts

#endif // C_CONTRACTS_PROVE_H
