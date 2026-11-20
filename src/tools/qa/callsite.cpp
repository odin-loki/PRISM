// prism-qa callsite-discharge: the whole-build call-site discharge of the automatic pointer harness
// (docs/HARNESS_GENERATOR.md "Whole-build call-site discharge"). Reads the --pir-discharge-out records of every unit
// of one build configuration (call graphs, harness plans, call-site checks, candidates), builds the call graph,
// grounds each candidate over it, and writes one decision per candidate plus the work still to do (the call sites of
// external functions in other units, call-graph facts of units not yet run). The decisions are new claims composed
// from pir results (each a solver verdict); no recorded verdict is rewritten.
#include "qa.hpp"

#include "../../prism/pir/discharge.hpp"

#include <fstream>
#include <iostream>

namespace prism::qa {

namespace fs = std::filesystem;

int callsite_discharge_main(const Args& args) {
    pir::discharge::WholeBuildOptions o;
    fs::path out;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const auto& a = args[i];
        auto val = [&]() -> std::string { return i + 1 < args.size() ? args[++i] : std::string(); };
        if (a == "--help" || a == "-h") {
            std::cout
                << "prism-qa callsite-discharge --in PATH [--in PATH ...] [--db compile_commands.json ...] [--complete-db]\n"
                   "                            [--link linux|none] [--config NAME] --out DIR\n"
                   "  PATH: --pir-discharge-out files, or directories of *.jsonl. --link linux: a non-exported external\n"
                   "  symbol is called only from its link unit (vmlinux or one module: an entry's \"link_units\", else\n"
                   "  -DMODULE / KBUILD_MODNAME of its command); default none: only static functions are closed.\n"
                   "  --complete-db: the databases list every object of every link unit (assembler, Rust); without it no\n"
                   "  external function is closed.\n"
                   "  Writes DIR/decisions.jsonl (one record per candidate: level proved | grounded | discharged | none,\n"
                   "  verdict, assumptions, reason), DIR/todo.jsonl (per unit: plans to check there, or facts to collect),\n"
                   "  DIR/callgraph.tsv and DIR/summary.json\n";
            return 0;
        }
        if (a == "--in") o.inputs.emplace_back(val());
        else if (a == "--db") o.compile_dbs.emplace_back(val());
        else if (a == "--complete-db") o.complete_db = true;
        else if (a == "--link") o.link_rule = val();
        else if (a == "--config") o.config = val();
        else if (a == "--out") out = val();
        else {
            std::cerr << "prism-qa callsite-discharge: unknown argument " << a << "\n";
            return 2;
        }
    }
    if (out.empty() || o.inputs.empty()) {
        std::cerr << "prism-qa callsite-discharge: --in and --out are required\n";
        return 2;
    }
    if (o.link_rule != "linux" && o.link_rule != "none") {
        std::cerr << "prism-qa callsite-discharge: --link expects linux or none\n";
        return 2;
    }
    auto r = pir::discharge::whole_build(o);
    std::error_code ec;
    fs::create_directories(out, ec);
    {
        std::ofstream d(out / "decisions.jsonl", std::ios::trunc | std::ios::binary);
        for (const auto& j : r.decisions) d << j.dump() << "\n";
    }
    {
        std::ofstream t(out / "todo.jsonl", std::ios::trunc | std::ios::binary);
        for (const auto& j : r.todo) t << j.dump() << "\n";
    }
    {
        std::ofstream g(out / "callgraph.tsv", std::ios::trunc | std::ios::binary);
        g << "kind\tcaller\tcallee\tresolved\tsites\n";
        for (const auto& e : r.edges)
            g << e.value("kind", "") << "\t" << e.value("caller", "") << "\t" << e.value("callee", "") << "\t"
              << e.value("resolved", "") << "\t" << e.value("sites", 0) << "\n";
    }
    r.summary["errors"] = r.errors;
    std::ofstream(out / "summary.json", std::ios::trunc) << r.summary.dump(2) << "\n";
    std::cout << r.summary.dump() << "\n";
    for (const auto& e : r.errors) std::cerr << "prism-qa callsite-discharge: " << e << "\n";
    return r.errors.empty() ? 0 : 1;
}

}  // namespace prism::qa
