//===- Prove.cpp - The CBMC tier
//-------------------------------------------===//

#include "Prove.h"
#include "CProver.h"

#include "clang/AST/ASTContext.h"
#include "llvm/ADT/ScopeExit.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/raw_ostream.h"

#include <chrono>
#include <csignal>
#include <thread>

using namespace clang;

namespace ccontracts {
namespace {

struct Command {
  int Code = 0;
  std::string Out;
  std::string Err;
  bool Ran = false; ///< False if the program could not be started at all.
};

std::string readFile(llvm::StringRef Path) {
  auto Buf = llvm::MemoryBuffer::getFile(Path);
  return Buf ? (*Buf)->getBuffer().str() : std::string();
}

/// Runs \p Prog, capturing both streams.
///
/// Everything below shells out: the lowering is the preprocessor's job, the
/// instrumentation is goto-instrument's, and the proof is CBMC's. Reproducing
/// any of them in-process would be a second implementation to keep honest.
Command run(llvm::StringRef Prog, llvm::ArrayRef<llvm::StringRef> Args,
            unsigned Timeout, llvm::StringRef WorkDir, llvm::StringRef Tag) {
  Command R;
  llvm::ErrorOr<std::string> Exe = llvm::sys::findProgramByName(Prog);
  if (!Exe) {
    R.Err = ("'" + Prog + "' is not on PATH").str();
    return R;
  }

  // A capture file per step, never a shared one. A step that dies without
  // writing anything would otherwise be read as whatever the step before it
  // printed, and a stale VERIFICATION line is the worst thing this tool could
  // report: a crash that looks like an answer.
  llvm::SmallString<128> OutPath(WorkDir), ErrPath(WorkDir);
  llvm::sys::path::append(OutPath, Tag.str() + ".out");
  llvm::sys::path::append(ErrPath, Tag.str() + ".err");
  llvm::sys::fs::remove(OutPath);
  llvm::sys::fs::remove(ErrPath);
  std::optional<llvm::StringRef> Redirects[] = {
      llvm::StringRef(""), llvm::StringRef(OutPath), llvm::StringRef(ErrPath)};

  llvm::SmallVector<llvm::StringRef> Argv;
  Argv.push_back(*Exe);
  Argv.append(Args.begin(), Args.end());

  std::string Msg;
  bool Failed = false;
  R.Code = llvm::sys::ExecuteAndWait(*Exe, Argv, std::nullopt, Redirects,
                                     Timeout, /*MemoryLimit=*/0, &Msg, &Failed);
  R.Out = readFile(OutPath);
  R.Err = Failed ? Msg : readFile(ErrPath);
  R.Ran = !Failed;
  return R;
}

/// Runs every installed solver on the same goto binary and keeps the first
/// answer.
///
/// COST.md in the fork measures up to 20x between the built-in SAT path and an
/// SMT solver, in EITHER direction depending on whether the buffer extents stay
/// symbolic, which makes a static preference a coin flip on a job that can run
/// for an hour. It is not only speed: cbmc 6.11 with --z3 aborts outright on
/// the loop-contract binaries here, so a tool that picked one solver would
/// report a crash where the other solver has a proof. Racing spends cores,
/// which are cheap, instead of wall time, which is not.
Command solve(llvm::StringRef Goto, llvm::StringRef Entry,
              llvm::ArrayRef<llvm::StringRef> Flags, unsigned Timeout,
              llvm::StringRef WorkDir, llvm::StringRef Tag,
              llvm::StringRef Only, std::string &Winner) {
  struct Racer {
    std::string Name;
    std::string Flag;
    std::string Out;
    std::string Err;
    llvm::sys::ProcessInfo PI;
    bool Done = false;
  };

  std::vector<Racer> Racers;
  auto add = [&](llvm::StringRef Name, llvm::StringRef Flag) {
    if (!Only.empty() && Only != Name)
      return;
    llvm::SmallString<128> Out(WorkDir), Err(WorkDir);
    llvm::sys::path::append(Out, Tag.str() + "-" + Name.str() + ".out");
    llvm::sys::path::append(Err, Tag.str() + "-" + Name.str() + ".err");
    llvm::sys::fs::remove(Out);
    llvm::sys::fs::remove(Err);
    Racers.push_back({Name.str(),
                      Flag.str(),
                      std::string(Out),
                      std::string(Err),
                      {},
                      false});
  };
  // Named rather than left as the default, so the log says which one won.
  add("sat", "");
  for (llvm::StringRef Name : {"z3", "bitwuzla", "cvc5"})
    if (llvm::sys::findProgramByName(Name))
      add(Name, ("--" + Name).str());

  Command Result;
  if (Racers.empty()) {
    Result.Err = ("no solver called '" + Only + "' is installed").str();
    return Result;
  }

  llvm::ErrorOr<std::string> Exe = llvm::sys::findProgramByName("cbmc");
  if (!Exe) {
    Result.Err = "'cbmc' is not on PATH";
    return Result;
  }

  for (Racer &R : Racers) {
    llvm::SmallVector<llvm::StringRef> Argv = {*Exe, Goto, "--function", Entry};
    if (!R.Flag.empty())
      Argv.push_back(R.Flag);
    Argv.append(Flags.begin(), Flags.end());
    std::optional<llvm::StringRef> Redirects[] = {
        llvm::StringRef(""), llvm::StringRef(R.Out), llvm::StringRef(R.Err)};
    std::string Msg;
    bool Failed = false;
    R.PI = llvm::sys::ExecuteNoWait(*Exe, Argv, std::nullopt, Redirects,
                                    /*MemoryLimit=*/0, &Msg, &Failed);
    R.Done = Failed;
  }

  auto Start = std::chrono::steady_clock::now();
  const Racer *Won = nullptr;
  int WinningCode = 0;
  while (!Won) {
    bool AllDone = true;
    for (Racer &R : Racers) {
      if (R.Done)
        continue;
      std::string Msg;
      llvm::sys::ProcessInfo W =
          llvm::sys::Wait(R.PI, /*SecondsToWait=*/0u, &Msg, nullptr,
                          /*Polling=*/true);
      if (W.Pid == 0) {
        AllDone = false;
        continue;
      }
      R.Done = true;
      std::string Log = readFile(R.Out);
      // A counterexample is definitive however much else was left undecided:
      // the trace exists. "Proved" is only a proof if every property was
      // decided, so a SUCCESSFUL run with an UNKNOWN property is not a winner.
      if (W.ReturnCode == 10 ||
          (W.ReturnCode == 0 && !llvm::StringRef(Log).contains(": UNKNOWN"))) {
        Won = &R;
        WinningCode = W.ReturnCode;
        break;
      }
      // Anything else is a front-end error, a crash or a solver giving up. Let
      // the others keep running.
    }
    if (Won || AllDone)
      break;
    if (std::chrono::steady_clock::now() - Start >
        std::chrono::seconds(Timeout))
      break;
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }

  for (Racer &R : Racers) {
    if (R.Done || (Won && &R == Won))
      continue;
    ::kill(R.PI.Pid, SIGKILL);
    llvm::sys::Wait(R.PI, /*SecondsToWait=*/std::nullopt);
  }

  if (Won) {
    Winner = Won->Name;
    Result.Ran = true;
    Result.Code = WinningCode;
    Result.Out = readFile(Won->Out);
    Result.Err = readFile(Won->Err);
    return Result;
  }

  // Nobody finished. Which phase each one reached says whether the solver was
  // even the problem: a run still inside symbolic execution is not rescued by a
  // different solver and needs a smaller harness instead.
  std::string Report;
  llvm::raw_string_ostream RS(Report);
  for (Racer &R : Racers) {
    std::string Log = readFile(R.Out);
    RS << "  " << R.Name << ": "
       << (R.Done ? "exited without a verdict" : "still running") << "\n";
    for (llvm::StringRef L : llvm::split(Log, '\n'))
      if (L.contains("Bounded Model Checking") || L.contains("Passing problem"))
        RS << "    last phase: " << L.trim() << "\n";
  }
  Result.Err = Report;
  return Result;
}

bool haveProgram(llvm::StringRef Name) {
  return static_cast<bool>(llvm::sys::findProgramByName(Name));
}

void replaceAll(std::string &S, llvm::StringRef From, llvm::StringRef To) {
  for (size_t P = S.find(From.str()); P != std::string::npos;
       P = S.find(From.str(), P + To.size()))
    S.replace(P, From.size(), To.str());
}

/// The compile command, reduced to what the preprocessor needs.
///
/// -std and the include and define flags decide what the translation unit is,
/// so they have to travel; the rest is code generation and diagnostics, which
/// this pass has no use for and which `cc` may not even accept.
std::vector<std::string>
preprocessorArgs(const std::vector<std::string> &Args) {
  static constexpr llvm::StringLiteral WithValue[] = {
      "-I", "-D", "-U", "-include", "-isystem", "-idirafter", "-iquote", "-F"};
  static constexpr llvm::StringLiteral Standalone[] = {"-nostdinc", "-undef",
                                                       "-C", "-P"};
  std::vector<std::string> Out;
  for (size_t I = 0; I < Args.size(); ++I) {
    llvm::StringRef A = Args[I];
    if (A.starts_with("-std=") || A.starts_with("--std=")) {
      Out.push_back(A.str());
      continue;
    }
    bool Taken = false;
    for (llvm::StringRef Flag : WithValue) {
      if (A == Flag && I + 1 < Args.size()) {
        Out.push_back(A.str());
        Out.push_back(Args[++I]);
        Taken = true;
        break;
      }
      if (A.starts_with(Flag) && A.size() > Flag.size()) {
        Out.push_back(A.str());
        Taken = true;
        break;
      }
    }
    if (Taken)
      continue;
    for (llvm::StringRef Flag : Standalone)
      if (A == Flag) {
        Out.push_back(A.str());
        break;
      }
  }
  return Out;
}

/// The verdict line CBMC printed, or empty if it printed none.
llvm::StringRef verdict(llvm::StringRef Log) {
  for (llvm::StringRef Needle :
       {"VERIFICATION SUCCESSFUL", "VERIFICATION FAILED"})
    if (Log.contains(Needle))
      return Needle;
  return {};
}

void printFailures(llvm::StringRef Log, llvm::raw_ostream &OS) {
  llvm::SmallVector<llvm::StringRef> Lines;
  Log.split(Lines, '\n');
  unsigned Shown = 0;
  for (llvm::StringRef L : Lines) {
    if (!L.ends_with(": FAILURE"))
      continue;
    if (Shown++ == 8) {
      OS << "    ... more; rerun with --verbose\n";
      break;
    }
    OS << "    " << L.trim() << "\n";
  }
}

} // namespace

int runProve(const Contract &C, llvm::StringRef File,
             const std::vector<std::string> &Args, const ProveOptions &Opts,
             bool Unparsed) {
  // Progress goes to stdout and diagnostics to stderr, and the two are not
  // flushed in a fixed order relative to each other unless this says so. A
  // warning that lands three lines away from what it is about is a warning
  // nobody connects to anything.
  llvm::outs().SetUnbuffered();
  llvm::raw_ostream &OS = llvm::outs();
  llvm::raw_ostream &ES = llvm::errs();
  llvm::StringRef Fn = Opts.Function;

  if (!C.Fn) {
    ES << "error: no contract on '" << Fn << "' in " << File << "\n";
    if (Unparsed)
      ES << "note: " << File
         << " did not parse. c-contracts reports contracts, not compile "
            "errors -- build the file first and fix what the compiler says.\n";
    return 2;
  }

  for (llvm::StringRef Tool :
       {Opts.CC, std::string("goto-cc"), std::string("goto-instrument"),
        std::string("cbmc")})
    if (!haveProgram(Tool)) {
      ES << "error: '" << Tool
         << "' is not on PATH; prove needs CBMC 6 or newer\n";
      return 2;
    }

  llvm::SmallString<128> Work;
  if (llvm::sys::fs::createUniqueDirectory("c-contracts-prove", Work)) {
    ES << "error: could not create a working directory\n";
    return 2;
  }
  llvm::scope_exit Cleanup([&] {
    if (Opts.KeepWork)
      OS << "work kept in " << Work << "\n";
    else
      llvm::sys::fs::remove_directories(Work);
  });
  auto inWork = [&](llvm::StringRef Name) {
    llvm::SmallString<128> P(Work);
    llvm::sys::path::append(P, Name);
    return std::string(P);
  };

  // A pointer the contract says is valid to touch, with nothing saying how big
  // the object behind it is, is the one shape that reliably fails for a reason
  // that is not a bug in the user's code. Say so before spending a solver on
  // it.
  for (const MissingFresh &M : findWritesWithoutFresh(C))
    ES << "warning: " << M.Clause << " says " << M.Pointer << " is "
       << (M.IsWrite ? "writable" : "readable")
       << ", which claims nothing about what object it points into. Add pre("
       << (M.IsWrite ? "fresh(" : "fresh(") << M.Pointer << ", " << M.Size
       << ")) beside it, or the proof reports failures that are not defects.\n";

  // A hand-written entry point wins over the generated one: auto-generation
  // cannot know a project's own allocation shape, and the escape hatch has to
  // be a file the author can read and edit rather than a flag.
  llvm::SmallString<128> ProofFile(Opts.ProofDir);
  llvm::sys::path::append(ProofFile, Fn.str() + ".proof.c");
  bool HandWritten = llvm::sys::fs::exists(ProofFile);
  llvm::StringRef Source = HandWritten ? llvm::StringRef(ProofFile) : File;
  if (HandWritten)
    OS << "entry point: " << ProofFile << " (hand written)\n";

  std::string HarnessName = ("__contract_harness_" + Fn).str();
  std::string VacuityName = ("__contract_vacuity_" + Fn).str();

  //--------------------------------------------------------------- lower
  std::vector<std::string> CppArgs = preprocessorArgs(Args);
  llvm::SmallVector<llvm::StringRef> Argv = {"-E", "-DC_CONTRACTS_CPROVER"};
  for (const std::string &A : CppArgs)
    Argv.push_back(A);
  Argv.push_back(Source);
  Argv.push_back("-o");
  std::string TuPath = inWork("tu.i");
  Argv.push_back(TuPath);

  Command Cpp = run(Opts.CC, Argv, Opts.Timeout, Work, "cpp");
  if (Cpp.Code != 0) {
    ES << "error: preprocessing " << Source << " failed:\n";
    ES << Cpp.Err;
    ES << "note: pass the defines the project's own build passes, after --\n";
    return 2;
  }

  std::string Tu = readFile(TuPath);
  // CBMC models memcpy and memmove under their plain names; Apple's and glibc's
  // headers route them through builtins that have no body there.
  replaceAll(Tu, "__builtin_memcpy", "memcpy");
  replaceAll(Tu, "__builtin_memmove", "memmove");

  std::vector<std::string> Clauses = extractClauses(Tu, /*Canonical=*/false);
  if (Clauses.empty()) {
    ES << "error: no contract clauses lowered -- is " << Source
       << " including c_contracts.h?\n";
    return 2;
  }
  OS << "lowered " << Clauses.size() << " clause(s):\n";
  for (const std::string &Cl : Clauses)
    OS << "  " << Cl << "\n";

  //--------------------------------------------------------------- entry point
  Harness H;
  if (!HandWritten && Opts.Caller.empty()) {
    std::string Err;
    if (!emitHarness(C, HarnessName, VacuityName, Opts.Bounds, H, Err)) {
      ES << "error: " << Err << "\n";
      return 2;
    }
    for (const std::string &W : H.Warnings)
      ES << "warning: " << W << "\n";
  }

  // Two translation units, not one. CBMC checks every assertion in the goto
  // program it is handed, whether or not the entry point can reach it, so a
  // probe that asserts false has to be compiled apart from the proof it is
  // there to validate -- otherwise it fails the proof itself.
  auto prepare = [&](llvm::StringRef Entry, llvm::StringRef Name,
                     std::string &Path) -> bool {
    std::string Text;
    llvm::raw_string_ostream P(Text);
    // The generated entry point calls CBMC builtins that no ordinary
    // translation unit declares. Redeclaring one the source already has is
    // legal C; declaring one it does not is the only way this compiles.
    for (auto [Decl, Symbol] :
         {std::pair{"void __CPROVER_assume(int);", "__CPROVER_assume"},
          std::pair{"void *__CPROVER_allocate(unsigned long, int);",
                    "__CPROVER_allocate"},
          std::pair{"void __CPROVER_assert(int, const char *);",
                    "__CPROVER_assert"}})
      if (!llvm::StringRef(Tu).contains(Symbol))
        P << Decl << "\n";
    P << Tu << "\n";
    // A static inline nobody calls is dropped before instrumentation.
    // Referencing it constrains nothing; the symbol just has to exist.
    P << "void *__contract_keep_" << Fn << " = (void *)&" << Fn << ";\n";
    P << Entry;

    Path = inWork(Name);
    std::error_code EC;
    llvm::raw_fd_ostream F(Path, EC);
    if (EC) {
      ES << "error: could not write " << Path << "\n";
      return false;
    }
    F << Text;
    return true;
  };

  std::string PreparedPath;
  if (!prepare(H.Text, "prepared.c", PreparedPath))
    return 2;

  //--------------------------------------------------------------- compile
  std::string GotoPath = inWork("a.goto");
  Command Cc = run("goto-cc", {PreparedPath, "-o", GotoPath}, Opts.Timeout,
                   Work, "goto-cc");
  if (Cc.Code != 0 || !llvm::sys::fs::exists(GotoPath)) {
    ES << "error: goto-cc could not compile the lowered unit:\n" << Cc.Err;
    return 2;
  }

  // --apply-loop-contracts has to run first and alone: --enforce-contract
  // refuses with "Loops remain in function" even when every loop is annotated.
  std::string LoopPath = inWork("l.goto");
  Command Loops =
      run("goto-instrument", {"--apply-loop-contracts", GotoPath, LoopPath},
          Opts.Timeout, Work, "loops");
  if (Loops.Code != 0 || !llvm::sys::fs::exists(LoopPath))
    LoopPath = GotoPath;

  //--------------------------------------------------------------- mode
  std::string Entry;
  std::string Target = LoopPath;
  std::string Mode;

  if (!Opts.Caller.empty()) {
    std::string CallerPath = inWork("r.goto");
    Command R = run("goto-instrument",
                    {"--replace-call-with-contract", Fn, LoopPath, CallerPath},
                    Opts.Timeout, Work, "replace");
    if (R.Code != 0 || !llvm::sys::fs::exists(CallerPath)) {
      ES << "error: goto-instrument could not replace calls to '" << Fn
         << "' with its contract:\n"
         << R.Err;
      return 2;
    }
    Target = CallerPath;
    Entry = Opts.Caller;
    Mode = "caller (preconditions are obligations)";
  } else {
    bool WantEnforce = Opts.Mode != "harness" && !HandWritten;
    if (Opts.Mode == "enforce" && HandWritten) {
      ES << "error: --mode=enforce builds the entry point from the contract, "
            "so it cannot use "
         << ProofFile << "\n";
      return 2;
    }
    if (WantEnforce) {
      std::string EnfPath = inWork("e.goto");
      Command E =
          run("goto-instrument", {"--enforce-contract", Fn, LoopPath, EnfPath},
              Opts.Timeout, Work, "enforce");
      llvm::StringRef Log =
          E.Err.empty() ? llvm::StringRef(E.Out) : llvm::StringRef(E.Err);
      bool Refused = Log.contains("Reason:") ||
                     !llvm::sys::fs::exists(EnfPath) || E.Code != 0;
      if (!Refused) {
        Target = EnfPath;
        Entry = Fn.str();
        Mode = "enforce (frame checked)";
      } else if (Opts.Mode == "enforce") {
        ES << "error: goto-instrument will not check " << Fn << "'s frame:\n";
        for (llvm::StringRef L : llvm::split(Log, '\n'))
          if (L.contains("Reason:"))
            ES << "  " << L.trim() << "\n";
        if (Log.contains("Loops remain")) {
          ES << "  every loop in " << Fn
             << " needs its own contract before its frame can be checked:\n";
          Command S = run("goto-instrument", {"--show-loops", GotoPath},
                          Opts.Timeout, Work, "show-loops");
          for (llvm::StringRef L : llvm::split(S.Out, '\n'))
            if (L.contains(Fn) && L.starts_with("Loop "))
              ES << "    " << L.trim() << "\n";
          ES << "  add to each: assigns (...) loop_invariant (...) "
                "decreases (...)\n";
        }
        return 2;
      } else {
        OS << "note: the frame is not checked -- ";
        if (Log.contains("Loops remain"))
          OS << "a loop in " << Fn << " carries no contract";
        else
          OS << "goto-instrument declined to instrument " << Fn;
        OS << ", so the proof runs from the generated entry point instead\n";
      }
    }
    if (Entry.empty()) {
      Entry = HarnessName;
      Mode = HandWritten ? "harness (hand written, frame not checked)"
                         : "harness (frame not checked)";
    }
  }
  OS << "mode: " << Mode << "\n";

  //--------------------------------------------------------------- flags
  llvm::SmallVector<llvm::StringRef> Common;
  std::string UnwindValue = std::to_string(Opts.Unwind);
  if (Opts.Unwind) {
    Common.push_back("--unwind");
    Common.push_back(UnwindValue);
    // Loud rather than quiet: a bound too small to reach the end of a loop is
    // the classic proof that proves nothing, and it looks exactly like a proof.
    Common.push_back("--unwinding-assertions");
  }
  bool HasCheck = false;
  for (const std::string &F : Opts.CBMCFlags) {
    Common.push_back(F);
    HasCheck |= llvm::StringRef(F).contains("-check");
  }
  // Every defect this project has found is a pointer formed outside its object.
  // --pointer-overflow-check is deliberately not in the default: it did not
  // finish in forty minutes on the wildcopy proof.
  if (!HasCheck) {
    Common.push_back("--bounds-check");
    Common.push_back("--pointer-check");
  }

  //--------------------------------------------------------------- vacuity
  // A suite that passes while proving nothing looks exactly like a suite that
  // works, forever, which is why this is on by default.
  if (Opts.Vacuity && !HandWritten && Opts.Caller.empty()) {
    std::string ProbeSrc, ProbeGoto = inWork("v.goto");
    if (!prepare(H.VacuityText, "vacuity.c", ProbeSrc))
      return 2;
    Command VCc = run("goto-cc", {ProbeSrc, "-o", ProbeGoto}, Opts.Timeout,
                      Work, "vacuity-cc");
    if (VCc.Code != 0 || !llvm::sys::fs::exists(ProbeGoto)) {
      ES << "error: goto-cc could not compile the vacuity probe:\n" << VCc.Err;
      return 2;
    }
    std::string ProbeWinner;
    Command Probe = solve(ProbeGoto, VacuityName, {}, Opts.Timeout, Work,
                          "vacuity", Opts.Solver, ProbeWinner);
    llvm::StringRef Verdict = verdict(Probe.Out);
    if (Verdict == "VERIFICATION SUCCESSFUL") {
      ES << "error: " << Fn
         << "'s preconditions are unsatisfiable -- nothing can call it, so a "
            "proof about it proves nothing\n";
      if (Opts.Verbose)
        ES << Probe.Out;
      return 1;
    }
    if (Verdict.empty()) {
      ES << "warning: the vacuity probe did not finish; the preconditions are "
            "unchecked\n";
      if (Opts.Verbose)
        ES << Probe.Out << Probe.Err;
    }
    // Nothing is printed when the probe passes. A check that narrates its own
    // success on every run is noise, and the whole reason this gate exists is
    // that its FAILURE is silent -- so that is the only thing worth a line.
    else if (Opts.Verbose)
      OS << "vacuity: " << Fn << "'s preconditions are satisfiable\n";
  } else if (Opts.Vacuity && HandWritten) {
    OS << "vacuity: not checked -- a hand-written entry point carries its own "
          "assumptions\n";
  }

  //--------------------------------------------------------------- prove
  std::string Winner;
  Command Proof = solve(Target, Entry, Common, Opts.Timeout, Work, "proof",
                        Opts.Solver, Winner);
  if (Opts.Verbose)
    OS << Proof.Out << Proof.Err;

  llvm::StringRef Verdict = verdict(Proof.Out);
  if (Verdict.empty()) {
    ES << "error: no solver reached a verdict for '" << Entry << "' within "
       << Opts.Timeout << "s\n"
       << Proof.Err;
    return 2;
  }
  OS << "solved by " << Winner << "\n";
  if (Verdict == "VERIFICATION FAILED" && !Opts.Verbose)
    printFailures(Proof.Out, ES);
  OS << Fn << ": " << Verdict << "\n";
  return Verdict == "VERIFICATION SUCCESSFUL" ? 0 : 1;
}

} // namespace ccontracts
