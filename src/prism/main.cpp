#include "prism/exec.hpp"
#include "prism/exit.hpp"
#include "prism/log.hpp"
#include "prism/solver.hpp"
#include "prism/membudget.hpp"
#include "prism/ai_proof.hpp"
#include "prism/ai_assist.hpp"
#include "prism/cli.hpp"
#include "prism/library.hpp"
#include "prism/config.hpp"
#include "prism/pipeline.hpp"
#include "prism/pir.hpp"
#include "prism/svcomp.hpp"
#include "prism/exec.hpp"
#include "prism/sandbox.hpp"

#include <array>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

int main(int argc, char** argv) {
    (void)prism::membudget::budget_mb();  // fix the budget and malloc arenas before any thread starts
    using namespace prism;
    // Ctrl-C / SIGTERM (a scorer's timeout) also kills the solvers, checkers
    // and compilers PRISM started: they run in process groups of their own
    prism::exec::install_interrupt_cleanup();
    // Roadmap 9.2: `prism prove FILE.lean THEOREM` (Lean proof search).
    if (argc > 1 && std::string_view(argv[1]) == "prove") return ai::prove_main(argc - 1, argv + 1);
    // Report-level subcommands (roadmap 9.3 / 9.4): they read report.json and
    // never change a verdict.
    if (argc > 1) {
        const std::string sub = argv[1];
        if (sub == "z3-worker") return solver::z3_worker_main(argc - 1, argv + 1);
        if (sub == "regress") return ai::regress_main(argc - 2, argv + 2);
        if (sub == "ask") return ai::ask_main(argc - 2, argv + 2);
        if (sub == "draft") return ai::draft_main(argc - 2, argv + 2);
        // Roadmap 6.3: PRISM as an SV-COMP verifier (docs/SVCOMP.md).
        if (sub == "svcomp") return svcomp::svcomp_main(argc - 2, argv + 2);
        if (sub == "triage") return ai::triage_main(argc - 2, argv + 2);
        // docs/PROOF_LIBRARY.md: earlier verdicts as data (import, vet, stats, lookup)
        if (sub == "library") return library::library_main(argc - 2, argv + 2);
    }
    auto parsed = parse_cli(std::span<const char* const>(argv + 1, argc - 1));
    if (!parsed.value) {
        return prism::fail_usage("", parsed.message, "(prism --help lists the options)");
    }
    if (parsed->help) {
        std::cout << cli_usage();
        return prism::code(prism::Exit::Ok);
    }
    if (parsed->version) {
        std::cout << "prism " << PRISM_VERSION << " (C++ engine)\n";
        return prism::code(prism::Exit::Ok);
    }
    if (parsed->list_stages) {
        for (auto* p = STAGE_ORDER; *p; ++p) std::cout << *p << "\n";
        return prism::code(prism::Exit::Ok);
    }
    auto& cfg = parsed->cfg;
    const auto& fail_on = parsed->fail_on;
    const auto& pir_vcs_src = parsed->pir_vcs_src;
    const auto& solve_smt2 = parsed->solve_smt2;
    const bool z3_only = parsed->z3_only;

    if (!pir_vcs_src.empty()) {
        std::cout << prism::pir::unit_vcs_json(std::filesystem::absolute(pir_vcs_src), cfg,
                                               std::filesystem::absolute(cfg.out))
                  << "\n";
        return prism::code(prism::Exit::Ok);
    }
    if (!solve_smt2.empty()) {
        std::ifstream in(solve_smt2, std::ios::binary);
        if (!in) {
            return prism::fail("", prism::log::cat("cannot read ", solve_smt2));
        }
        std::ostringstream ss;
        ss << in.rdbuf();
        std::cout << prism::pir::solve_smt2_json(ss.str(), cfg, z3_only) << "\n";
        return prism::code(prism::Exit::Ok);
    }

    cfg.root = std::filesystem::absolute(parsed->path);
    cfg.out = std::filesystem::absolute(cfg.out);
    // A mistyped PATH is not an empty, clean tree (Laws 1 and 7).
    if (std::error_code ec; !std::filesystem::exists(cfg.root, ec)) {
        return prism::fail("", "PATH does not exist: " + cfg.root.string());
    }

    auto report = run_pipeline(cfg);
    std::cout << "confidence " << report.confidence
              << "  (vis " << report.visibility << " x ans " << report.answer
              << " x res " << report.resolution << ")\n";
    std::cout << "report " << (cfg.out / "report.md").string() << "  (sarif "
              << (cfg.out / "report.sarif").string() << ")\n";
    for (auto& s : report.stages) {
        if (s.status == "NOTRUN") {
            static bool hdr = false;
            if (!hdr) {
                std::cout << "NOTRUN:\n";
                hdr = true;
            }
            std::cout << "  " << s.name << ": " << s.detail << "  " << s.install << "\n";
        }
    }
    std::vector<const Finding*> failed;
    for (auto& s : report.stages)
        for (auto& f : s.findings)
            if (f.status == "FAILED" || f.status == "CRASH") failed.push_back(&f);
    std::sort(failed.begin(), failed.end(), [](auto* a, auto* b) {
        int ka = a->status == "CRASH" ? prism::code(prism::Exit::Ok) : prism::code(prism::Exit::Failed);
        int kb = b->status == "CRASH" ? prism::code(prism::Exit::Ok) : prism::code(prism::Exit::Failed);
        if (ka != kb) return ka < kb;
        if (a->stage != b->stage) return a->stage < b->stage;
        return a->file < b->file;
    });
    for (std::size_t i = 0; i < failed.size() && i < 30; ++i) {
        auto* f = failed.at(i);
        std::cout << "  " << f->status << " " << f->stage << " " << f->file << ":"
                  << (f->line ? *f->line : 0) << " "
                  << (f->function ? *f->function : "") << " " << f->message << "\n";
    }
    return exit_code(report, fail_on);
}
