// Solver and bound prediction: train, measure, export (roadmap 3.1 / 9.1 / 9.3 / 9.7).
//
// Input logs (JSON lines):
//
//   * solver runs: solve_runs.jsonl from the collection doctest
//     (PRISM_PREDICT_COLLECT=DIR prism_tests -tc="ai-assist predict collect*",
//     source directories listed in DIR/roots.txt): one line per verification
//     condition with the wall time of every portfolio member run ALONE
//     (runs: {z3: {kind, wall_s}, ...}; a timeout is a censored time); or the
//     production <cache>/solve_log.jsonl written by every portfolio solve
//     (only members that finished have a time; not used as training targets).
//   * bound runs: bound_runs.jsonl, per function the verdict and seconds at
//     unwind 1, 2, 4, 8, 16.
//   * --repeat-*-log: the held-out files collected a second time, for the
//     run-to-run noise a model must beat.
//
// Measurement is on a held-out split (by source file, deterministic hash):
//
//   * solver choice (quick metric): the member given the head start, and the
//     seconds that member alone needs;
//   * solver replay (the gate, sched.cpp): the portfolio scheduler replayed on
//     the alone-times with k = 1, 2, 3, 5 cores for the rules, the per-bucket
//     history, GBDT, censored (AFT) GBDT, k-NN and a winner classifier; total
//     time, timeouts, median / p90 latency, bootstrap interval;
//   * bound: the unwind tried first; agreement with the verdict at unwind 16,
//     total seconds, BOUNDED / no-verdict counts, time to first counterexample.
//
// The exported model carries "enabled": true only when it beats the baselines
// on the held-out split by more than max(5%, run-to-run noise) with no more
// timeouts (roadmap 9.7); src/prism/solver/predict.cpp ignores a model that is
// not enabled. The model file is src/prism/solver/predict_default.json when it
// becomes the built-in model (CMake embeds it).

#include "prism_ai.hpp"
#include "pycompat.hpp"

#include "prism/solver_predict.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <set>
#include <sstream>

namespace prism_ai::predict {
using prism::solver::gbdt::Model;

const std::vector<int> UNWINDS = {1, 2, 4, 8, 16};
const std::vector<int> REPLAY_KS = {1, 2, 3, 5};  // cores: sequential, and the portfolio with k members at once
const std::vector<int> DECIDE_KS = {2, 3, 5};     // max_parallel is max(2, hardware threads)
constexpr double EPS = 1e-3;

namespace {

double num(const ojson& v, double dflt = 0.0) {
    if (v.is_number()) return v.get<double>();
    if (v.is_string()) {
        try {
            return std::stod(v.get<std::string>());
        } catch (...) {
        }
    }
    return dflt;
}

bool truthy(const ojson& v) {
    if (v.is_null()) return false;
    if (v.is_boolean()) return v.get<bool>();
    if (v.is_number()) return v.get<double>() != 0.0;
    if (v.is_string()) return !v.get<std::string>().empty();
    return !v.empty();
}

std::string join(const std::vector<std::string>& v, const std::string& sep) {
    std::string s;
    for (std::size_t i = 0; i < v.size(); ++i) s += (i ? sep : "") + v[i];
    return s;
}

const std::vector<std::string>& query_features() { return prism::solver::predict::query_feature_names(); }
const std::vector<std::string>& function_features() { return prism::solver::predict::function_feature_names(); }

}  // namespace

// ------------------------------------------------------------------ solver data
std::optional<double> SolverRow::time(const std::string& m) const {
    for (const auto& [n, s] : times)
        if (n == m) return s;
    return std::nullopt;
}

std::vector<SolverRow> solver_rows(const std::vector<ojson>& recs) {
    // Both log formats -> {key, bucket, x, times{member: s}}.
    std::vector<SolverRow> rows;
    for (const auto& r : recs) {
        if (!r.is_object() || !r.contains("features") || !r["features"].is_array() ||
            r["features"].size() != query_features().size())
            continue;
        SolverRow row;
        if (r.contains("runs")) {  // collection: every member ran alone
            if (r["runs"].is_object())
                for (auto it = r["runs"].begin(); it != r["runs"].end(); ++it)
                    row.times.emplace_back(it.key(), it.value().is_object() && it.value().contains("wall_s")
                                                         ? num(it.value()["wall_s"])
                                                         : 0.0);
        } else if (r.contains("times") && r["times"].is_object()) {  // production log: finished members only
            for (auto it = r["times"].begin(); it != r["times"].end(); ++it) row.times.emplace_back(it.key(), num(it.value()));
        }
        if (row.times.empty()) continue;
        if (r.contains("file") && truthy(r["file"])) row.key = py(r["file"]);
        else if (r.contains("hash") && truthy(r["hash"])) row.key = py(r["hash"]);
        row.bucket = r.contains("bucket") ? py(r["bucket"]) : "";
        for (const auto& v : r["features"]) row.x.push_back(num(v));
        rows.push_back(std::move(row));
    }
    return rows;
}

SolverResult measure_solver(const std::vector<SolverRow>& rows, int n_trees) {
    SolverResult out;
    auto& res = out.res;
    std::set<std::string> mset;
    for (const auto& r : rows)
        for (const auto& [m, s] : r.times) mset.insert(m);
    const std::vector<std::string> members(mset.begin(), mset.end());
    std::vector<const SolverRow*> full, train, test;
    for (const auto& r : rows) {
        bool all = true;
        for (const auto& m : members) all = all && r.time(m).has_value();
        if (!all) continue;
        full.push_back(&r);
        (held_out(r.key) ? test : train).push_back(&r);
    }
    res["members"] = members;
    res["queries"] = full.size();
    res["train"] = train.size();
    res["test"] = test.size();
    if (train.size() < 10 || test.size() < 5 || members.size() < 2) {
        res["note"] = "not enough complete solver runs to measure";
        res["enabled"] = false;
        return out;
    }
    std::vector<std::vector<double>> xs;
    for (auto* r : train) xs.push_back(r->x);
    for (const auto& m : members) {
        std::vector<double> ys;
        for (auto* r : train) ys.push_back(std::log(*r->time(m) + EPS));
        Model g;
        g.n_trees = n_trees;
        g.fit(xs, ys);
        out.models.emplace_back(m, std::move(g));
    }
    // history baseline: per-bucket mean seconds per member (the current scheduler)
    std::map<std::string, std::map<std::string, std::vector<double>>> hist;
    for (auto* r : train)
        for (const auto& [m, s] : r->times) hist[r->bucket][m].push_back(s);
    auto argmin = [&](const std::function<double(const std::string&)>& key) {
        const std::string* best = nullptr;
        double bv = 0.0;
        for (const auto& m : members) {
            const double v = key(m);
            if (!best || v < bv) best = &m, bv = v;
        }
        return *best;
    };
    auto pick_rules = [&](const SolverRow& r) {
        return argmin([&](const std::string& m) { return sched::rule_estimate(m, r.x); });
    };
    auto pick_hist = [&](const SolverRow& r) {
        auto it = hist.find(r.bucket);
        if (it == hist.end() || it->second.empty()) return pick_rules(r);
        const auto& hb = it->second;
        return argmin([&](const std::string& m) {
            auto h = hb.find(m);
            if (h == hb.end()) return sched::rule_estimate(m, r.x);
            double s = 0.0;
            for (double v : h->second) s += v;
            return s / static_cast<double>(h->second.size());
        });
    };
    auto pick_model = [&](const SolverRow& r) {
        return argmin([&](const std::string& m) {
            for (const auto& [n, g] : out.models)
                if (n == m) return g.predict(r.x);
            return 0.0;
        });
    };
    auto pick_oracle = [&](const SolverRow& r) { return argmin([&](const std::string& m) { return *r.time(m); }); };
    const std::vector<std::pair<std::string, std::function<std::string(const SolverRow&)>>> picks = {
        {"rules", pick_rules}, {"history", pick_hist}, {"gbdt", pick_model}, {"oracle", pick_oracle}};
    ojson pol = ojson::object();
    for (const auto& [name, pick] : picks) {
        double total = 0.0, acc = 0.0;
        for (auto* r : test) {
            const auto m = pick(*r);
            total += *r->time(m);
            acc += m == pick_oracle(*r) ? 1.0 : 0.0;
        }
        pol[name] = ojson{{"seconds", py_round(total, 3)},
                          {"accuracy", py_round(acc / static_cast<double>(test.size()), 3)}};
    }
    res["policies"] = pol;
    const double rs = pol["rules"]["seconds"].get<double>(), hs = pol["history"]["seconds"].get<double>();
    const double base = std::min(rs, hs);
    res["baseline"] = hs <= rs ? "history" : "rules";
    // Beat the better baseline by at least 5% of its time on the held-out split.
    res["enabled"] = pol["gbdt"]["seconds"].get<double>() < 0.95 * base;
    return out;
}

// ------------------------------------------------------------------ bound data
std::vector<BoundRow> bound_rows(const std::vector<ojson>& recs) {
    std::vector<BoundRow> rows;
    for (const auto& r : recs) {
        if (!r.is_object() || !r.contains("features") || !r["features"].is_array() ||
            r["features"].size() != function_features().size())
            continue;
        std::map<int, BoundRun> runs;
        if (r.contains("runs") && r["runs"].is_array())
            for (const auto& u : r["runs"]) {
                if (!u.is_object() || !u.contains("unwind")) continue;
                runs[static_cast<int>(num(u["unwind"]))] =
                    BoundRun{u.contains("status") ? py(u["status"]) : "None", u.contains("seconds") ? num(u["seconds"]) : 0.0};
            }
        bool all = true;
        for (int u : UNWINDS) all = all && runs.count(u);
        if (!all) continue;
        const auto final_status = runs[UNWINDS.back()].status;
        if (final_status == "ERROR" || final_status == "UNKNOWN" || final_status == "TIMEOUT") continue;
        // Label: the smallest unwind from which the verdict equals the one at
        // 16. A BOUNDED verdict is never "reached early": bounded depth is its
        // assurance, so its label is the largest bound.
        int label = UNWINDS.back();
        if (final_status != "BOUNDED")
            for (int u : UNWINDS) {
                bool same = true;
                for (int v : UNWINDS)
                    if (v >= u && runs[v].status != final_status) same = false;
                if (same) {
                    label = u;
                    break;
                }
            }
        BoundRow row;
        row.key = r.contains("file") ? py(r["file"]) : "";
        row.function = r.contains("function") ? py(r["function"]) : "";
        for (const auto& v : r["features"]) row.x.push_back(num(v));
        row.runs = std::move(runs);
        row.final_status = final_status;
        row.label = label;
        rows.push_back(std::move(row));
    }
    return rows;
}

ojson policy(const std::vector<BoundRow>& rows, const std::function<int(const BoundRow&)>& choose) {
    int agree = 0, n_failed = 0, bounded = 0, no_verdict = 0, contradictions = 0;
    double seconds = 0.0, ttfc = 0.0;
    for (const auto& r : rows) {
        const auto& run = r.runs.at(choose(r));
        seconds += run.seconds;
        agree += run.status == r.final_status;
        bounded += run.status == "BOUNDED";
        no_verdict += run.status == "ERROR" || run.status == "UNKNOWN" || run.status == "TIMEOUT";
        // A check on the data, not on the policy: a proof at the chosen depth
        // while unwind 16 finds a counterexample would be a pir bug (every
        // unwind's verdict must stand on its own).
        contradictions += run.status.rfind("PROVED", 0) == 0 && r.final_status == "FAILED";
        if (r.final_status == "FAILED") {
            ++n_failed;
            // time to the first counterexample: try u, else escalate to 16
            ttfc += run.seconds + (run.status == "FAILED" ? 0.0 : r.runs.at(UNWINDS.back()).seconds);
        }
    }
    const double n = static_cast<double>(std::max<std::size_t>(1, rows.size()));
    return ojson{{"agreement", py_round(agree / n, 4)},       {"seconds", py_round(seconds, 3)},
                 {"time_to_first_cex", py_round(ttfc, 3)},    {"failed_functions", n_failed},
                 {"bounded", bounded},                        {"no_verdict", no_verdict},
                 {"contradictions", contradictions}};
}

int snap(double y) {
    // Predicted log2 unwind -> the smallest listed unwind >= 2**y.
    const double want = std::pow(2.0, std::max(0.0, y));
    for (int u : UNWINDS)
        if (u >= want - 1e-9) return u;
    return UNWINDS.back();
}

namespace {
// k nearest training functions (standardised features): the largest label
// among them (a larger bound is the safe side).
std::function<int(const BoundRow&)> knn_bound(const std::vector<BoundRow>& train, std::size_t k = 9) {
    const double n = static_cast<double>(std::max<std::size_t>(1, train.size()));
    const std::size_t dims = function_features().size();
    std::vector<double> mu(dims), sd(dims);
    for (std::size_t i = 0; i < dims; ++i) {
        double s = 0.0;
        for (const auto& r : train) s += r.x[i];
        mu[i] = s / n;
    }
    for (std::size_t i = 0; i < dims; ++i) {
        double s = 0.0;
        for (const auto& r : train) s += std::pow(r.x[i] - mu[i], 2.0);
        sd[i] = std::max(1e-6, std::sqrt(s / n));
    }
    auto pts = std::make_shared<std::vector<std::pair<std::vector<double>, int>>>();
    for (const auto& r : train) {
        std::vector<double> z;
        for (std::size_t i = 0; i < r.x.size(); ++i) z.push_back((r.x[i] - mu[i]) / sd[i]);
        pts->emplace_back(std::move(z), r.label);
    }
    return [pts, mu, sd, k](const BoundRow& r) {
        std::vector<double> z;
        for (std::size_t i = 0; i < r.x.size(); ++i) z.push_back((r.x[i] - mu[i]) / sd[i]);
        std::vector<std::pair<double, std::size_t>> d;
        for (std::size_t j = 0; j < pts->size(); ++j) {
            double s = 0.0;
            for (std::size_t i = 0; i < std::min(z.size(), (*pts)[j].first.size()); ++i)
                s += std::pow(z[i] - (*pts)[j].first[i], 2.0);
            d.emplace_back(s, j);
        }
        std::sort(d.begin(), d.end());
        int best = 0;
        for (std::size_t i = 0; i < d.size() && i < k; ++i) best = std::max(best, (*pts)[d[i].second].second);
        return best;
    };
}
}  // namespace

BoundResult measure_bound(const std::vector<BoundRow>& rows, int n_trees, const std::vector<BoundRow>* repeat) {
    BoundResult out;
    auto& res = out.res;
    std::vector<BoundRow> train, test;
    for (const auto& r : rows) (held_out(r.key) ? test : train).push_back(r);
    res["functions"] = rows.size();
    res["train"] = train.size();
    res["test"] = test.size();
    if (train.size() < 10 || test.size() < 5) {
        res["note"] = "not enough bound runs to measure";
        res["enabled"] = false;
        return out;
    }
    std::vector<std::vector<double>> xs;
    std::vector<double> ys;
    for (const auto& r : train) {
        xs.push_back(r.x);
        ys.push_back(std::log2(static_cast<double>(r.label)));
    }
    Model model;
    model.n_trees = n_trees;
    model.fit(xs, ys);
    const std::string fixed_default = "fixed-" + std::to_string(DEFAULT_UNWIND);
    ojson pol = ojson::object();
    pol[fixed_default] = policy(test, [](const BoundRow&) { return DEFAULT_UNWIND; });
    pol["fixed-" + std::to_string(UNWINDS.back())] = policy(test, [](const BoundRow&) { return UNWINDS.back(); });
    pol["gbdt"] = policy(test, [&](const BoundRow& r) { return snap(model.predict(r.x)); });
    pol["knn"] = policy(test, knn_bound(train));
    pol["oracle"] = policy(test, [](const BoundRow& r) { return r.label; });
    res["policies"] = pol;
    if (repeat && !repeat->empty()) {
        // Run-to-run noise: the same held-out functions timed twice at unwind 8.
        std::map<std::pair<std::string, std::string>, const BoundRow*> rep;
        for (const auto& r : *repeat) rep[{r.key, r.function}] = &r;
        std::vector<BoundRow> common, again;
        for (const auto& r : test)
            if (auto it = rep.find({r.key, r.function}); it != rep.end()) {
                common.push_back(r);
                again.push_back(*it->second);
            }
        const double a = policy(common, [](const BoundRow&) { return DEFAULT_UNWIND; })["seconds"].get<double>();
        const double b = policy(again, [](const BoundRow&) { return DEFAULT_UNWIND; })["seconds"].get<double>();
        res["noise"] = ojson{{"functions", common.size()}, {"run1_s", a}, {"run2_s", b},
                             {"rel", py_round(std::abs(a - b) / std::max(a, 1e-9), 4)}};
    }
    const auto& b = pol[fixed_default];
    const auto& g = pol["gbdt"];
    double margin = 0.05;
    if (res.contains("noise")) margin = std::max(margin, res["noise"]["rel"].get<double>());
    // Never trade verdicts for time: same or better agreement, no more BOUNDED
    // or no-verdict outcomes, AND less time by more than the noise.
    res["enabled"] = g["agreement"].get<double>() >= b["agreement"].get<double>() &&
                     g["bounded"].get<int>() <= b["bounded"].get<int>() &&
                     g["no_verdict"].get<int>() <= b["no_verdict"].get<int>() &&
                     g["seconds"].get<double>() < (1 - margin) * b["seconds"].get<double>();
    out.model = std::move(model);
    return out;
}

ojson build_model(const SolverResult* solver, const BoundResult* bound, const ReplayDecision* replay) {
    // With a replay measurement (sched run_all + decide) the solver part is
    // enabled only when the replay chose a model; its trees are exported (the
    // C++ loader evaluates GBDT / AFT trees alike).
    ojson m = ojson::object();
    m["schema"] = 1;
    m["kind"] = "prism-gbdt";
    m["query_features"] = query_features();
    m["function_features"] = function_features();
    m["note"] = "trained by prism_ai predict; enabled only when the held-out measurement beats the baseline (roadmap 9.7)";
    bool enabled = false;
    ojson metrics = ojson::object();
    if (solver) {
        metrics["solver"] = solver->res;
        if (!replay && solver->enabled() && !solver->models.empty()) {
            ojson s = ojson::object();
            for (const auto& [name, g] : solver->models) s[name] = g.to_json();
            m["solvers"] = s;
            enabled = true;
        }
    }
    if (replay) {
        ojson rp = replay->replay.res;
        rp["chosen"] = replay->chosen ? ojson(*replay->chosen) : ojson(nullptr);
        rp["why"] = replay->why;
        metrics["replay"] = rp;
        if (replay->chosen) {
            const auto family = replay->chosen->substr(0, replay->chosen->find('-'));
            auto it = replay->replay.gbdt_models.find(family);
            if (it != replay->replay.gbdt_models.end()) {
                ojson s = ojson::object();
                for (const auto& [name, g] : it->second->models) s[name] = g.to_json();
                m["solvers"] = s;
                m["head_start"] = !replay->chosen->ends_with("-order");
                enabled = true;
            }
        }
    }
    if (bound) {
        metrics["bound"] = bound->res;
        if (bound->enabled() && bound->model) {
            m["bound"] = bound->model->to_json();
            enabled = true;
        }
    }
    m["enabled"] = enabled;
    m["metrics"] = metrics;
    return m;
}

std::string markdown(const ojson& model) {
    std::vector<std::string> out = {"| part | policy | held-out result |", "|---|---|---|"};
    const ojson empty = ojson::object();
    const auto& met = model.contains("metrics") ? model["metrics"] : empty;
    const auto& s = met.contains("solver") ? met["solver"] : empty;
    auto opt = [](const ojson& o, const char* k, const std::string& dflt) { return o.contains(k) ? py(o[k]) : dflt; };
    if (s.contains("policies") && s["policies"].is_object())
        for (auto it = s["policies"].begin(); it != s["policies"].end(); ++it) {
            const auto& p = it.value();
            out.push_back("| solver choice (" + opt(s, "test", "None") + " VCs) | " + it.key() + " | " + py(p["seconds"]) +
                          " s, picks the fastest " + py_fixed(p["accuracy"].get<double>() * 100, 1) + "% |");
        }
    const auto& b = met.contains("bound") ? met["bound"] : empty;
    if (b.contains("policies") && b["policies"].is_object())
        for (auto it = b["policies"].begin(); it != b["policies"].end(); ++it) {
            const auto& p = it.value();
            out.push_back("| unwind (" + opt(b, "test", "None") + " functions) | " + it.key() + " | agreement " +
                          py_fixed(p["agreement"].get<double>() * 100, 1) + "%, " + py(p["seconds"]) + " s, first cex " +
                          py(p["time_to_first_cex"]) + " s, BOUNDED " + opt(p, "bounded", "") + ", no verdict " +
                          opt(p, "no_verdict", "") + " |");
        }
    if (b.contains("noise")) {
        const auto& nz = b["noise"];
        out.push_back("| unwind noise | fixed-" + std::to_string(DEFAULT_UNWIND) + " timed twice (" + py(nz["functions"]) +
                      " functions) | " + py(nz["run1_s"]) + " s vs " + py(nz["run2_s"]) + " s |");
    }
    out.push_back("");
    if (met.contains("replay") && truthy(met["replay"])) {
        const auto& rp = met["replay"];
        out.push_back(sched::markdown(rp));
        const std::string chosen = rp.contains("chosen") && truthy(rp["chosen"]) ? py(rp["chosen"]) : "none";
        out.push_back("replay decision: " + chosen + " (" + opt(rp, "why", "") + ")");
        out.push_back("");
    }
    const bool solvers = model.contains("solvers") && truthy(model["solvers"]);
    out.push_back(std::string("solver model enabled: ") + (solvers ? "True" : "False") +
                  "; bound model enabled: " + opt(b, "enabled", "False") +
                  "; model file enabled: " + opt(model, "enabled", "None"));
    return join(out, "\n") + "\n";
}

int predict_main(int argc, char** argv, std::ostream& out, std::ostream& err) {
    std::vector<fs::path> solve_logs, bound_logs, rep_solve, rep_bound, e2e;
    std::optional<fs::path> model_out, md_out;
    int trees = 60;
    for (int i = 0; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) throw std::invalid_argument(a + " needs a value");
            return argv[++i];
        };
        try {
            if (a == "--solve-log") solve_logs.emplace_back(next());
            else if (a == "--bound-log") bound_logs.emplace_back(next());
            else if (a == "--repeat-solve-log") rep_solve.emplace_back(next());
            else if (a == "--repeat-bound-log") rep_bound.emplace_back(next());
            else if (a == "--out") model_out = next();
            else if (a == "--trees") trees = std::stoi(next());
            else if (a == "--markdown") md_out = next();
            else if (a == "--end-to-end") e2e.emplace_back(next());
            else throw std::invalid_argument("unknown argument " + a);
        } catch (const std::exception& e) {
            err << "prism_ai predict: " << e.what() << "\n"
                << "usage: prism_ai predict --out MODEL.json [--solve-log F]... [--bound-log F]... "
                   "[--repeat-solve-log F]... [--repeat-bound-log F]... [--trees N] [--markdown F] "
                   "[--end-to-end E2E.json]...\n";
            return 2;
        }
    }
    if (!model_out) {
        err << "prism_ai predict: --out is required\n";
        return 2;
    }
    std::vector<SolverRow> srows;
    for (const auto& p : solve_logs)
        for (auto& r : solver_rows(load_jsonl(p))) srows.push_back(std::move(r));
    std::vector<BoundRow> brows, brep;
    for (const auto& p : bound_logs)
        for (auto& r : bound_rows(load_jsonl(p))) brows.push_back(std::move(r));
    for (const auto& p : rep_bound)
        for (auto& r : bound_rows(load_jsonl(p))) brep.push_back(std::move(r));
    std::optional<SolverResult> solver;
    if (!srows.empty()) solver = measure_solver(srows, trees);
    std::optional<ReplayDecision> replay;
    const auto rrows = solve_logs.empty() ? std::vector<sched::Row>{} : sched::load(solve_logs);
    // The replay needs every member timed alone (the collection format).
    std::size_t complete = 0;
    for (const auto& r : rrows) complete += r.runs.size() >= 2;
    if (!rrows.empty() && complete >= 30) {
        std::vector<sched::Row> rep;
        if (!rep_solve.empty()) rep = sched::load(rep_solve);
        ReplayDecision d;
        d.replay = sched::run_all(rrows, REPLAY_KS, trees, rep_solve.empty() ? nullptr : &rep);
        std::tie(d.chosen, d.why) = sched::decide(d.replay.res, DECIDE_KS);
        replay = std::move(d);
    }
    std::optional<BoundResult> bound;
    if (!brows.empty()) bound = measure_bound(brows, trees, brep.empty() ? nullptr : &brep);
    auto model = build_model(solver ? &*solver : nullptr, bound ? &*bound : nullptr, replay ? &*replay : nullptr);
    if (!e2e.empty()) {
        ojson ee = ojson::object();
        for (std::size_t i = 0; i < e2e.size(); ++i) {
            std::ifstream in(e2e[i], std::ios::binary);
            try {
                ee["run" + std::to_string(i + 1)] = ojson::parse(in).at("summary");
            } catch (const std::exception& e) {
                err << "prism_ai predict: " << e2e[i].string() << ": no e2e summary (" << e.what() << ")\n";
                return 1;
            }
        }
        model["metrics"]["end_to_end"] = ee;
    }
    {
        std::ofstream f(*model_out, std::ios::binary);
        f << model.dump(1);
        if (!f) {
            err << "prism_ai predict: cannot write " << model_out->string() << "\n";
            return 1;
        }
    }
    const auto md = markdown(model);
    if (md_out) std::ofstream(*md_out, std::ios::binary) << md;
    out << md;
    return 0;
}

}  // namespace prism_ai::predict
