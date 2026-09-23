// Tooling for tools/solver_bench.py (roadmap 3.1 exit criterion, docs/SOLVERS.md):
//   prism --pir-vcs FILE.c --out DIR   write every pir VC of FILE as SMT-LIB2
//   prism --solve-smt2 FILE.smt2        answer one VC with the solver library
// Neither is a pipeline stage: they print JSON and never write a report.

#include "prism/laws.hpp"
#include "prism/pir.hpp"

#include <nlohmann/json.hpp>

#include <fstream>
#include <sstream>

#ifdef PRISM_HAS_Z3
#  include "prism/solver.hpp"
#  include <z3++.h>
#endif

namespace prism::pir {
namespace fs = std::filesystem;

std::string unit_vcs_json(const fs::path& src, const Config& cfg, const fs::path& out_dir) {
    nlohmann::json j{{"file", src.string()}, {"unwind", cfg.unwind}, {"functions", nlohmann::json::array()}};
    auto fe = find_frontend(cfg);
    std::string err;
    std::vector<FoldedUb> folded;
    std::vector<std::pair<int, int>> sshl;
    auto ir = lower_to_ir(fe, src, std::max(10.0, cfg.timeout), err, &folded, &sshl);
    if (!ir) {
        j["error"] = err;
        return j.dump(1);
    }
    auto mod = ir::parse_module(*ir);
    TranslateOptions topt;
    topt.signed_shl = sshl;
    topt.folded = folded;
    std::error_code ec;
    fs::create_directories(out_dir, ec);
    const auto unit = src.filename().string();
    for (auto& irf : mod.functions) {
        ir::DISub sub;
        if (auto it = mod.subprograms.find(irf.dbg); it != mod.subprograms.end()) sub = it->second;
        if (sub.artificial || irf.name.starts_with("__cxx_global_var_init") || irf.name.starts_with("_GLOBAL__"))
            continue;
        if (!sub.file.empty() && fs::path(sub.file).filename().string() != unit) continue;
        const std::string name = sub.name.empty() ? irf.name : sub.name;
        nlohmann::json fj{{"function", name}, {"vcs", nlohmann::json::array()}};
        auto tr = translate(mod, irf, topt);
        if (!tr.fn) {
            fj["status"] = tr.status.empty() ? std::string(laws::NEEDS_HARNESS) : tr.status;
            fj["reason"] = tr.reason;
            j["functions"].push_back(fj);
            continue;
        }
        auto vcs = pir_vcs(*tr.fn, cfg.unwind);
        int k = 0;
        for (auto& vc : vcs) {
            auto p = out_dir / (unit + "." + irf.name + "." + std::to_string(k++) + ".smt2");
            std::ofstream(p, std::ios::binary) << vc.smt2;
            fj["vcs"].push_back({{"path", p.string()}, {"kind", vc.kind}, {"prop", vc.prop}, {"cls", vc.cls},
                                 {"line", vc.line}});
        }
        j["functions"].push_back(fj);
    }
    return j.dump(1);
}

std::string solve_smt2_json(const std::string& smt2, const Config& cfg, bool z3_only) {
    nlohmann::json j;
#ifdef PRISM_HAS_Z3
    try {
        z3::context c;
        auto v = c.parse_string(smt2.c_str());
        z3::expr_vector xs(c);
        for (unsigned i = 0; i < v.size(); ++i) xs.push_back(v[i]);
        z3::expr f = xs.empty() ? c.bool_val(true) : z3::mk_and(xs);
        solver::SolveOptions so;
        so.timeout_s = cfg.timeout;
        so.portfolio = !z3_only;
        so.certified = cfg.certified;
        // Timing, not answers: the query cache is off; --solver-cache only
        // chooses where the scheduler's solve-time history lives.
        so.cache_dir = cfg.solver_cache.string();
        so.use_cache = false;
        auto r = solver::solve(c, f, so);
        j = {{"kind", std::string(solver::kind_name(r.kind))},
             {"status", std::string(solver::verdict_status(r))},
             {"winner", r.winner},
             {"certified", r.certified},
             {"certificate_info", r.certificate_info},
             {"cnf_sha256", r.cnf_sha256},
             {"cache_hit", r.cache_hit},
             {"bucket", r.bucket},
             {"wall_s", r.wall_s},
             {"note", r.note}};
    } catch (const z3::exception& ex) {
        j = {{"kind", "error"}, {"note", std::string("z3: ") + ex.msg()}};
    }
#else
    (void)smt2;
    (void)cfg;
    (void)z3_only;
    j = {{"kind", "error"}, {"status", std::string(laws::NOTRUN)}, {"note", "z3 not built"}};
#endif
    return j.dump(1);
}

}  // namespace prism::pir
