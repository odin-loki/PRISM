// Solver and bound prediction (roadmap 9.1 / 9.3). See
// include/prism/solver_predict.hpp. The model decides scheduling order only;
// the solver still decides every answer.

#include "prism/solver_predict.hpp"

#include "prism/pir.hpp"

#include "internal.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <map>
#include <mutex>

namespace prism::solver::predict {
namespace fs = std::filesystem;
using json = nlohmann::json;

const std::vector<std::string>& query_feature_names() {
    static const std::vector<std::string> v = {"log2_bv_width", "log10_nodes", "log10_consts", "arrays", "fp",
                                               "uf", "arith", "quant", "bv_mul_div"};
    return v;
}

const std::vector<std::string>& function_feature_names() {
    static const std::vector<std::string> v = {"params", "log2_vars", "blocks", "log2_stmts", "checks", "back_edges",
                                               "phis", "mul_div", "max_width", "log2_max_const"};
    return v;
}

namespace {

struct Loaded {
    fs::file_time_type mtime{};
    bool ok = false;
    bool enabled = false;
    json j;
};
std::mutex g_mu;
std::map<std::string, Loaded> g_cache;

double eval_tree(const json& n, const std::vector<double>& x, int depth = 0) {
    if (depth > 16 || !n.is_object()) return 0.0;
    if (n.contains("v")) return n["v"].get<double>();
    auto f = n.value("f", -1);
    double t = n.value("t", 0.0);
    double xv = (f >= 0 && static_cast<std::size_t>(f) < x.size()) ? x[static_cast<std::size_t>(f)] : 0.0;
    return eval_tree(xv <= t ? n["l"] : n["r"], x, depth + 1);
}

double eval_target(const json& tgt, const std::vector<double>& x) {
    double s = tgt.value("base", 0.0);
    const double lr = tgt.value("lr", 0.1);
    if (tgt.contains("trees") && tgt["trees"].is_array())
        for (const auto& t : tgt["trees"]) s += lr * eval_tree(t, x);
    return s;
}

const Loaded* load(const fs::path& cache_dir) {
    if (const char* off = std::getenv("PRISM_SOLVER_PREDICT"); off && std::string(off) == "0") return nullptr;
    auto p = model_path(cache_dir);
    std::error_code ec;
    auto mt = fs::last_write_time(p, ec);
    if (ec) return nullptr;
    std::lock_guard<std::mutex> g(g_mu);
    auto& e = g_cache[p.string()];
    if (e.ok && e.mtime == mt) return &e;
    e = Loaded{};
    e.mtime = mt;
    try {
        std::ifstream in(p);
        e.j = json::parse(in);
        e.ok = e.j.is_object() && e.j.value("schema", 0) == 1 && e.j.value("kind", "") == "prism-gbdt";
        if (e.ok && e.j.contains("query_features") && e.j["query_features"] != json(query_feature_names())) e.ok = false;
        if (e.ok && e.j.contains("function_features") && e.j["function_features"] != json(function_feature_names()))
            e.ok = false;
        e.enabled = e.ok && e.j.value("enabled", false);
    } catch (...) {
        e.ok = false;
    }
    return e.ok ? &e : nullptr;
}

}  // namespace

fs::path model_path(const fs::path& cache_dir) {
    if (const char* m = std::getenv("PRISM_SOLVER_MODEL"); m && *m) return m;
    return cache_dir / "predict_model.json";
}

bool enabled(const fs::path& cache_dir) {
    auto* l = load(cache_dir);
    return l && l->enabled;
}

double gbdt_eval(const std::string& target_json, const std::vector<double>& x) {
    try {
        return eval_target(json::parse(target_json), x);
    } catch (...) {
        return 0.0;
    }
}

std::optional<double> seconds(const fs::path& cache_dir, const std::string& member, const std::vector<double>& x) {
    auto* l = load(cache_dir);
    if (!l || !l->enabled || !l->j.contains("solvers") || !l->j["solvers"].contains(member)) return std::nullopt;
    // Targets are log(seconds + 1e-3).
    return std::max(0.0, std::exp(eval_target(l->j["solvers"][member], x)) - 1e-3);
}

std::vector<double> function_features(const pir::Function& fn) {
    double stmts = 0, checks = 0, back = 0, phis = 0, muldiv = 0, maxw = 0, maxc = 0;
    for (std::size_t b = 0; b < fn.blocks.size(); ++b) {
        const auto& blk = fn.blocks[b];
        phis += static_cast<double>(blk.phis.size());
        for (const auto& s : blk.stmts) {
            ++stmts;
            if (s.kind == pir::Stmt::Check) ++checks;
            if (s.op == pir::Op::Mul || s.op == pir::Op::UDiv || s.op == pir::Op::SDiv || s.op == pir::Op::URem ||
                s.op == pir::Op::SRem)
                ++muldiv;
            for (const auto& a : s.args)
                if (a.is_const && a.bits) maxc = std::max(maxc, std::log2(static_cast<double>(a.bits) + 1.0));
        }
        for (int t : {blk.term.t, blk.term.f})
            if (t >= 0 && static_cast<std::size_t>(t) <= b) ++back;
    }
    for (const auto& v : fn.vars) maxw = std::max(maxw, static_cast<double>(v.width));
    return {static_cast<double>(fn.params.size()), std::log2(static_cast<double>(fn.vars.size()) + 1.0),
            static_cast<double>(fn.blocks.size()), std::log2(stmts + 1.0), checks, back, phis, muldiv, maxw, maxc};
}

int unwind_for(const fs::path& cache_dir, const std::vector<double>& x, int fallback, int cap) {
    auto* l = load(cache_dir);
    if (!l || !l->enabled || !l->j.contains("bound")) return fallback;
    // Target is log2(unwind); round up (a larger bound is the safe side).
    double y = eval_target(l->j["bound"], x);
    int u = static_cast<int>(std::ceil(std::exp2(std::clamp(y, 0.0, 10.0)) - 1e-9));
    return std::clamp(u, 1, cap);
}

#ifdef PRISM_HAS_Z3
std::vector<double> query_features(const Features& ft) {
    return {std::log2(static_cast<double>(ft.max_bv_width) + 1.0), std::log10(static_cast<double>(ft.nodes) + 1.0),
            std::log10(static_cast<double>(ft.consts) + 1.0), ft.arrays ? 1.0 : 0.0, ft.fp ? 1.0 : 0.0,
            ft.uf ? 1.0 : 0.0, ft.arith ? 1.0 : 0.0, ft.quant ? 1.0 : 0.0, ft.bv_mul_div ? 1.0 : 0.0};
}

void log_query(const fs::path& cache_dir, const Features& ft, const SolveResult& res,
               const std::vector<std::string>& raced, double wall_s) {
    static std::mutex mu;
    std::lock_guard<std::mutex> g(mu);
    const auto p = cache_dir / "solve_log.jsonl";
    std::error_code ec;
    if (fs::exists(p, ec) && fs::file_size(p, ec) > (32u << 20)) return;
    json line;
    line["schema"] = 1;
    line["hash"] = res.query_hash;
    line["bucket"] = res.bucket;
    line["features"] = query_features(ft);
    line["result"] = std::string(kind_name(res.kind));
    line["winner"] = res.winner;
    line["times"] = res.times;  // members that finished; the others are censored at wall_s
    line["raced"] = raced;
    line["wall_s"] = wall_s;
    fs::create_directories(cache_dir, ec);
    std::ofstream(p, std::ios::app) << line.dump() << "\n";
}
#endif

}  // namespace prism::solver::predict
