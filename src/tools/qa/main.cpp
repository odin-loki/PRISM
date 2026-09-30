// prism-qa: developer QA tools for PRISM (built by default, not shipped).
// Every command is in qa.hpp.

#include "qa.hpp"

#include "../../prism/proc.hpp"

#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {
struct Command {
    std::string_view name;
    int (*run)(const prism::qa::Args&);
    std::string_view what;
};

int assurance(const prism::qa::Args& a) { return prism::qa::assurance_main(a, prism::qa::repo_root()); }

constexpr Command COMMANDS[] = {
    {"triage-selfscan", prism::qa::triage_selfscan_main, "bucket a self-scan report.sarif (real / false alarm / out of scope)"},
    {"pir-vs-bmc", prism::qa::pir_vs_bmc_main, "bmc encoder vs pir stage agreement matrix and hard conflicts"},
    {"pir-lean-check", prism::qa::pir_lean_check_main, "C++ LLVM->PIR translator vs the proved Lean translator"},
    {"llvm-sem-vs-lli", prism::qa::llvm_sem_vs_lli_main, "formal LLVM semantics (Lean) vs lli"},
    {"libc-bounds", prism::qa::libc_bounds_main, "size-bound sweep of the libc model contract harnesses"},
    {"solver-bench", prism::qa::solver_bench_main, "portfolio vs Z3 alone on the conformance suite's pir VCs"},
    {"assurance-check", assurance, "every artefact docs/assurance cites exists"},
    {"docs-check", prism::qa::docs_check_main, "--help flags documented, anchors resolve, stage order listed"},
};

int usage(std::ostream& o, int rc) {
    o << "usage: prism-qa COMMAND [ARGS...]   (COMMAND --help for its options)\n";
    for (const auto& c : COMMANDS) o << "  " << c.name << std::string(18 - c.name.size(), ' ') << c.what << "\n";
    return rc;
}
}  // namespace

int main(int argc, char** argv) {
    // Ctrl-C / SIGTERM also kills the PRISM runs (and their solvers) started here
    prism::detail::install_child_cleanup();
    if (argc < 2) return usage(std::cerr, 2);
    const std::string_view cmd = argv[1];
    if (cmd == "-h" || cmd == "--help") return usage(std::cout, 0);
    prism::qa::Args args(argv + 2, argv + argc);
    for (const auto& c : COMMANDS)
        if (c.name == cmd) return c.run(args);
    std::cerr << "prism-qa: unknown command " << cmd << "\n";
    return usage(std::cerr, 2);
}
