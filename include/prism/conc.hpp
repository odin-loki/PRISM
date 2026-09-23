#pragma once

// conc: the concurrency stage (roadmap 2.6 "Threads and atomics",
// docs/CONCURRENCY.md).
//
// Bounded context-switch model checking by lazy sequentialisation (the
// Lazy-CSeq method, Inverso/Tomasco/Fischer/La Torre/Parlato): the threads of
// a harness (main, or any root function that calls pthread_create /
// thrd_create) are run round-robin for K rounds; in every round each thread
// resumes at its saved program counter and runs to a nondeterministically
// chosen context-switch point. Context-switch points sit before every visible
// operation (shared-memory access, lock, unlock, create, join, atomic).
//
// Front end: the PIR one (include/prism/pir.hpp). The LLVM module is rewritten
// so every visible operation becomes a named marker statement that
// pir::translate understands (a nondet read or an llvm.expect copy); the PIR
// UB/assertion instrumentation therefore applies in every thread unchanged.
//
// Verdicts: FAILED (data race, assertion/UB, deadlock, unlock of a mutex not
// held) with the interleaving as counterexample; BOUNDED ("no violation
// within K rounds") - never PROVED: a context-switch bound is not a proof.
// Anything not modelled (heap-shared data before the PIR memory model,
// non-SC memory orders, std::thread, thread arguments) is NEEDS-HARNESS with
// an "UNENCODED: ..." reason. Functions that create no threads: no findings.

#include "prism/config.hpp"
#include "prism/export.hpp"
#include "prism/models.hpp"
#include "prism/pir.hpp"

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace prism::conc {

struct Options {
    int rounds = 2;          // K: round-robin rounds (context-switch bound)
    int unwind = 4;          // loop unwinding bound inside each thread
    int max_threads = 4;     // created threads (the harness thread is extra)
    double timeout_s = 30.0; // per solver query
    int max_findings = 8;    // distinct violations reported per harness
    std::size_t max_nodes = 20000;  // unrolled nodes per thread
};

// A visible operation: a context-switch point is placed before each one.
struct VisOp {
    enum Kind { Load, Store, Rmw, Lock, TryLock, Unlock, MutexInit, Create, Join };
    Kind kind = Load;
    int var = -1;            // Load/Store/Rmw: shared variable
    int mutex = -1;          // Lock/TryLock/Unlock/MutexInit
    int thread = -1;         // Create: index of the created thread (>= 1)
    bool atomic = false;     // Load/Store/Rmw: an atomic (seq_cst) access
    unsigned width = 0;      // Load/Store/Rmw: access width; TryLock: result width
    uint64_t busy = 16;      // TryLock: result when the mutex is held (EBUSY / thrd_busy)
    int line = 0;
    std::string what;        // "load", "store", "atomicrmw add", "pthread_mutex_lock", ...
};

struct Shared {
    std::string name;        // IR global name (without '@')
    unsigned width = 0;
    uint64_t init = 0;
};

struct Thread {
    std::string entry;       // IR name of the entry function
    std::string name;        // source name
    int create_op = -1;      // VisOp index of the creating call (-1: the harness)
    pir::Function fn;        // translated body (markers name visible operations)
};

// The concurrent program extracted from one harness.
struct Program {
    std::string harness;              // IR name of the harness function
    std::vector<Shared> vars;
    std::vector<std::string> mutexes; // IR global names
    std::vector<VisOp> ops;
    std::vector<Thread> threads;      // [0] is the harness
    int atomic_mutex = -1;            // pseudo-mutex of SV-COMP atomic sections (-1: none)
};

struct Build {
    std::optional<Program> prog;
    std::string status;      // "" ok, else laws::NEEDS_HARNESS
    std::string reason;      // "UNENCODED: ..."
};

// Global variable definitions of a textual module (the IR parser keeps only
// functions): name -> (type text, initialiser text).
struct GlobalDef {
    std::string name;
    std::string type;
    std::string init;
    bool constant = false;
};
PRISM_API std::vector<GlobalDef> parse_globals(std::string_view ir_text);

// Root functions that (directly or through callees) create threads:
// "main" when it does, else every such function nobody calls.
PRISM_API std::vector<std::string> harnesses(const pir::ir::Module& m);
// Calls that show threads the stage cannot model (std::thread ...), by
// function: IR function name -> reason.
PRISM_API std::map<std::string, std::string> unsupported_threading(const pir::ir::Module& m);

PRISM_API Build build_program(const pir::ir::Module& m, std::string_view ir_text,
                              const std::string& harness, const Options& opt = {});

// One executed visible operation of a counterexample schedule.
struct Event {
    int round = 0;
    int thread = 0;
    int op = -1;
    int line = 0;
    std::string text;        // "T1 worker: store counter = 1 (line 8)"
};

struct Violation {
    std::string kind;        // "race" | "deadlock" | "unlock" | a PIR property ("assert", "ovf+", ...)
    std::string cls;         // taxonomy class
    std::string msg;
    int line = 0;
    int thread = 0;
    std::vector<Event> trace;
    std::string schedule;    // one-line rendering of trace
};

struct Result {
    std::string status;      // FAILED / BOUNDED / UNKNOWN / NEEDS-HARNESS / NOTRUN
    std::string message;
    std::vector<Violation> violations;
    std::map<std::string, std::string> extra;
};

PRISM_API Result check(const Program& prog, const Options& opt = {});

// The sequentialised program as readable pseudo-C (docs, debugging): one
// function per thread re-entered every round with a guarded jump to its saved
// program counter, and the round-robin driver.
PRISM_API std::string lazy_text(const Program& prog, const Options& opt = {});

// Whole-file convenience for tests: lower (clang/opt), extract, check every
// harness. Returns one Result per harness (keyed by source name).
PRISM_API std::map<std::string, Result> check_file(const std::filesystem::path& src, const Config& cfg,
                                                   const Options& opt = {});

// The conc stage: one finding per violation (FAILED) or per harness
// (BOUNDED / NEEDS-HARNESS / UNKNOWN). Units without threads: no findings.
PRISM_API std::vector<Finding> run_conc(const std::vector<std::filesystem::path>& sources, const Config& cfg);

}  // namespace prism::conc
