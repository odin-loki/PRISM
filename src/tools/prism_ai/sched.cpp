// Learned solver scheduling: replay, models, held-out evaluation (roadmap 3.1 / 9.3).
//
// Input: solve_runs.jsonl from the collection doctest
// (PRISM_PREDICT_COLLECT=DIR prism_tests -tc="ai-assist predict collect*"):
// one line per verification condition with the time of EVERY portfolio member
// run ALONE (runs: {member: {kind, wall_s}}). A member that timed out is a
// censored time (>= the timeout); unknown/error is a member that gave up
// without an answer (the ProbSAT walker on an unsat query, Bitwuzla on a
// formula it cannot read); a missing member does not take that query.
//
// Replay. simulate() runs the portfolio scheduler of
// src/prism/solver/portfolio.cpp on those alone-times: members sorted by their
// expected time, a head start for the leader (or for in-process Z3 when there
// is none), k slots (cores), the first definitive answer wins, the query
// times out at T. It assumes members on separate cores do not slow each
// other (the real portfolio pays some contention, docs/SOLVERS.md), and that
// CaDiCaL and Kissat each pay their own bit-blast (the real one shares it).
//
// Policies (every one only orders members and sets the head start; the answer
// is whatever the member that finishes first says, and the portfolio still
// validates every SAT model in Z3):
//
//   rules      the default today: feature rules, Z3 alone for 0.15 s;
//   history    per-bucket mean times and winners (what the scheduler does
//              once solve_times.json has entries for the bucket);
//   gbdt       one GBDT per member on log(seconds), a timeout counted as T
//              (the model the C++ loader reads);
//   aft        one GBDT per member with a censored (Tobit / accelerated
//              failure time) loss: a timeout says only ">= T";
//   knn        k nearest training queries (standardised features), the
//              median of their log times;
//   winner     a classifier for "which member answers first"; used only to
//              pick the first member, the rest in rule order.
//
// Split: held out by SOURCE FILE (deterministic hash of the path); identical
// VCs (same SMT-LIB2 hash) are kept once, in the file that sorts first, so no
// query is in both halves.

#include "prism_ai.hpp"
#include "pycompat.hpp"

#include "prism/config.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <limits>
#include <set>
#include <sstream>

namespace prism_ai {

std::string py(const ojson& v) {
    if (v.is_string()) return v.get<std::string>();
    if (v.is_boolean()) return v.get<bool>() ? "True" : "False";
    if (v.is_null()) return "None";
    if (v.is_number_integer()) return std::to_string(v.get<long long>());
    if (v.is_number_unsigned()) return std::to_string(v.get<unsigned long long>());
    if (v.is_number_float()) return py_repr(v.get<double>());
    return v.dump();
}

bool held_out(const std::string& key, double share) {
    const auto h = prism::sha256_hex(key).substr(0, 8);
    const auto v = std::stoull(h, nullptr, 16);
    return static_cast<double>(v % 1000) / 1000.0 < share;
}

std::vector<ojson> load_jsonl(const fs::path& p) {
    std::vector<ojson> out;
    std::ifstream in(p, std::ios::binary);
    for (std::string line; std::getline(in, line);) {
        auto b = line.find_first_not_of(" \t\r\n");
        if (b == std::string::npos) continue;
        try {
            out.push_back(ojson::parse(line));
        } catch (const std::exception&) {
            continue;
        }
    }
    return out;
}

namespace sched {

const std::vector<std::string> MEMBERS = {"z3", "bitwuzla", "cadical", "kissat", "sls"};

namespace {

int member_index(const std::string& m) {
    auto it = std::find(MEMBERS.begin(), MEMBERS.end(), m);
    return it == MEMBERS.end() ? 99 : static_cast<int>(it - MEMBERS.begin());
}

std::string str_of(const ojson& r, const char* key) {
    if (!r.is_object() || !r.contains(key)) return "";
    return py(r[key]);
}

double num(const ojson& v, double dflt) {
    if (v.is_number()) return v.get<double>();
    if (v.is_string()) {
        try {
            return std::stod(v.get<std::string>());
        } catch (...) {
        }
    }
    return dflt;
}

double sum(const std::vector<double>& v) {
    double s = 0.0;
    for (double x : v) s += x;
    return s;
}

}  // namespace

const Run* Row::run(const std::string& m) const {
    for (const auto& [n, r] : runs)
        if (n == m) return &r;
    return nullptr;
}

std::vector<Row> load_records(std::vector<ojson> recs) {
    std::stable_sort(recs.begin(), recs.end(), [](const ojson& a, const ojson& b) {
        return std::make_tuple(str_of(a, "file"), str_of(a, "function"), str_of(a, "vc")) <
               std::make_tuple(str_of(b, "file"), str_of(b, "function"), str_of(b, "vc"));
    });
    std::set<std::string> seen;
    std::vector<Row> rows;
    for (const auto& r : recs) {
        if (!r.is_object()) continue;
        const ojson runs = r.contains("runs") && r["runs"].is_object() ? r["runs"] : ojson::object();
        if (!r.contains("features") || !r["features"].is_array() || r["features"].size() != 9 || !runs.contains("z3"))
            continue;
        std::string sha = r.contains("sha") && !r["sha"].is_null() ? py(r["sha"]) : "";
        if (sha == "False" || sha == "0") sha.clear();  // Python: `r.get("sha") or ""`
        if (!sha.empty()) {
            if (seen.count(sha)) continue;
            seen.insert(sha);
        }
        Row row;
        row.T = r.contains("timeout_s") ? num(r["timeout_s"], 8.0) : 8.0;
        std::set<std::string> kinds;
        for (auto it = runs.begin(); it != runs.end(); ++it) {
            const auto& v = it.value();
            const std::string k = v.is_object() && v.contains("kind") ? py(v["kind"]) : "";
            const double s = v.is_object() && v.contains("wall_s") ? num(v["wall_s"], 0.0) : 0.0;
            Run run;
            if ((k == "sat" || k == "unsat") && s < row.T) run = {State::Ans, s};  // an answer after T: a stall
            else if (k == "timeout" || s >= row.T) run = {State::Cens, row.T};
            else run = {State::Gave, s};
            row.runs.emplace_back(it.key(), run);
            if (v.is_object() && v.contains("kind") && v["kind"].is_string() && (k == "sat" || k == "unsat"))
                kinds.insert(k);
        }
        row.key = str_of(r, "file");
        row.sha = sha;
        row.bucket = str_of(r, "bucket");
        for (const auto& v : r["features"]) row.x.push_back(num(v, 0.0));
        row.answer = kinds.size() == 1 ? *kinds.begin() : (kinds.empty() ? "none" : "disagree");
        rows.push_back(std::move(row));
    }
    return rows;
}

std::vector<Row> load(const std::vector<fs::path>& paths) {
    std::vector<ojson> recs;
    for (const auto& p : paths) {
        // Only well-formed lines; a blank line is not a record.
        std::ifstream in(p, std::ios::binary);
        for (std::string line; std::getline(in, line);) {
            try {
                recs.push_back(ojson::parse(line));
            } catch (const std::exception&) {
                continue;
            }
        }
    }
    return load_records(std::move(recs));
}

std::pair<std::vector<Row>, std::vector<Row>> split(const std::vector<Row>& rows) {
    std::pair<std::vector<Row>, std::vector<Row>> out;
    for (const auto& r : rows) (held_out(r.key) ? out.second : out.first).push_back(r);
    return out;
}

std::pair<double, bool> log_target(State st, double s, double T) {
    // A member that gave up never answers: for ordering it is as bad as a timeout.
    if (st == State::Ans) return {std::log(s + EPS), false};
    return {std::log(T + EPS), true};
}

double sls_budget(double T) { return std::max(1.0, 0.1 * T); }

std::pair<double, std::string> simulate(const Row& row, const Est& est, const std::optional<std::string>& lead,
                                        double lead_delay, int k, std::optional<double> Topt) {
    const double T = Topt ? *Topt : row.T;
    constexpr double inf = std::numeric_limits<double>::infinity();
    std::vector<std::string> present;
    for (const auto& m : MEMBERS)
        if (row.has(m)) present.push_back(m);
    std::vector<std::string> extra;
    for (const auto& [m, r] : row.runs)
        if (member_index(m) == 99) extra.push_back(m);
    std::sort(extra.begin(), extra.end());
    present.insert(present.end(), extra.begin(), extra.end());
    auto e_of = [&](const std::string& m) {
        auto it = est.find(m);
        return it == est.end() ? 1.5 : it->second;
    };
    std::vector<std::string> order = present;
    std::stable_sort(order.begin(), order.end(),
                     [&](const std::string& a, const std::string& b) { return e_of(a) < e_of(b); });
    auto index_of = [&](const std::string& m) {
        return static_cast<std::size_t>(std::find(order.begin(), order.end(), m) - order.begin());
    };
    std::map<std::string, double> not_before;
    for (const auto& m : order) not_before[m] = 0.0;
    if (lead && not_before.count(*lead) && order.size() > 1) {
        for (const auto& m : order)
            if (m != *lead) not_before[m] = lead_delay;
    } else if (order.size() > 1 && not_before.count("z3")) {
        for (const auto& m : order)
            if (m != "z3") not_before[m] = std::min(Z3_FIRST_S, 0.1 * T);
    }
    std::vector<std::string> started;  // start order
    std::map<std::string, double> ends, answer_at;
    auto is_started = [&](const std::string& m) { return ends.count(m) > 0; };
    double t = 0.0;
    while (true) {
        std::vector<std::string> running;
        for (const auto& m : started)
            if (ends[m] > t) running.push_back(m);
        for (const auto& m : order) {  // portfolio.cpp fill_slots
            if (is_started(m) || not_before[m] > t) continue;
            if (static_cast<int>(running.size()) >= k) break;
            started.push_back(m);
            const Run& r = *row.run(m);
            if (r.state == State::Ans) {
                ends[m] = t + r.s;
                answer_at[m] = t + r.s;
            } else if (r.state == State::Cens) {
                ends[m] = inf;
            } else {
                ends[m] = t + (m == "sls" ? std::max(r.s, sls_budget(T)) : r.s);
            }
            running.push_back(m);
        }
        std::optional<std::pair<std::string, double>> first;
        for (const auto& [m, at] : answer_at)
            if (!first || at < first->second || (at == first->second && index_of(m) < index_of(first->first)))
                first = std::make_pair(m, at);
        double nt = inf;
        for (const auto& [m, e] : ends)
            if (e > t) nt = std::min(nt, e);
        for (const auto& [m, nb] : not_before)
            if (!is_started(m) && nb > t) nt = std::min(nt, nb);
        if (first && first->second <= nt)
            return first->second < T ? std::make_pair(first->second, first->first) : std::make_pair(T, std::string());
        if (nt >= T || nt == inf) return {T, ""};
        t = nt;
    }
}

double rule_estimate(const std::string& member, const std::vector<double>& x) {
    const double width = std::pow(2.0, x[0]) - 1;
    const double nodes = std::pow(10.0, x[1]) - 1;
    const bool arrays = x[3] > 0.5, fp = x[4] > 0.5, uf = x[5] > 0.5, arith = x[6] > 0.5, quant = x[7] > 0.5,
               muldiv = x[8] > 0.5;
    const bool wide = width > 32, big = nodes > 2000;
    if (member == "z3") return (arrays || uf || arith || quant) ? 0.5 : (big || wide) ? 1.2 : 0.6;
    if (member == "bitwuzla") return (fp || wide || muldiv || big) ? 0.4 : 0.8;
    if (member == "cadical") return (muldiv && wide) ? 2.0 : big ? 0.9 : 1.1;
    if (member == "kissat") return (muldiv && wide) ? 2.2 : big ? 0.95 : 1.2;
    if (member == "sls") return 3.0;
    return 1.5;
}

std::optional<std::string> fastest(const Row& r) {
    std::optional<std::tuple<double, int, std::string>> best;
    for (const auto& [m, run] : r.runs) {
        if (run.state != State::Ans) continue;
        auto cand = std::make_tuple(run.s, member_index(m), m);
        if (!best || cand < *best) best = cand;
    }
    if (!best) return std::nullopt;
    return std::get<2>(*best);
}

// ------------------------------------------------------------------ models
GbdtModel::GbdtModel(const std::vector<Row>& train, bool censored, int n_trees) {
    name = censored ? "aft" : "gbdt";
    for (const auto& m : MEMBERS) {
        std::vector<std::vector<double>> xs;
        std::vector<double> ys;
        std::vector<bool> cs;
        for (const auto& r : train)
            if (const Run* run = r.run(m)) {
                auto [y, c] = log_target(run->state, run->s, r.T);
                xs.push_back(r.x);
                ys.push_back(y);
                cs.push_back(c);
            }
        if (xs.size() < 10) continue;
        prism::solver::gbdt::Model g;
        g.n_trees = n_trees;
        if (censored) g.fit_censored(xs, ys, cs);
        else g.fit(xs, ys);
        models.emplace_back(m, std::move(g));
    }
}

std::optional<double> GbdtModel::seconds(const std::string& member, const std::vector<double>& x) const {
    for (const auto& [m, g] : models)
        if (m == member) return std::max(0.0, std::exp(g.predict(x)) - EPS);
    return std::nullopt;
}

KnnModel::KnnModel(const std::vector<Row>& train, int k) : k_(k) {
    name = "knn";
    const double n = static_cast<double>(std::max<std::size_t>(1, train.size()));
    const std::size_t dims = train.empty() ? 9 : train[0].x.size();
    for (std::size_t i = 0; i < dims; ++i) {
        double s = 0.0;
        for (const auto& r : train) s += r.x[i];
        mu_.push_back(s / n);
    }
    for (std::size_t i = 0; i < dims; ++i) {
        double s = 0.0;
        for (const auto& r : train) s += std::pow(r.x[i] - mu_[i], 2.0);
        sd_.push_back(std::max(1e-6, std::sqrt(s / n)));
    }
    for (const auto& r : train) {
        std::vector<double> z;
        for (std::size_t i = 0; i < r.x.size(); ++i) z.push_back((r.x[i] - mu_[i]) / sd_[i]);
        std::vector<std::pair<std::string, double>> t;
        for (const auto& [m, run] : r.runs) t.emplace_back(m, log_target(run.state, run.s, r.T).first);
        pts_.emplace_back(std::move(z), std::move(t));
    }
}

const std::vector<std::size_t>& KnnModel::nn(const std::vector<double>& x) const {
    auto it = cache_.find(x);
    if (it != cache_.end()) return it->second;
    std::vector<double> z;
    for (std::size_t i = 0; i < x.size(); ++i) z.push_back((x[i] - mu_[i]) / sd_[i]);
    std::vector<std::pair<double, std::size_t>> d;
    for (std::size_t j = 0; j < pts_.size(); ++j) {
        double s = 0.0;
        for (std::size_t i = 0; i < std::min(z.size(), pts_[j].first.size()); ++i)
            s += std::pow(z[i] - pts_[j].first[i], 2.0);
        d.emplace_back(s, j);
    }
    std::sort(d.begin(), d.end());
    std::vector<std::size_t> out;
    for (std::size_t i = 0; i < d.size() && static_cast<int>(i) < k_; ++i) out.push_back(d[i].second);
    return cache_.emplace(x, std::move(out)).first->second;
}

std::optional<double> KnnModel::seconds(const std::string& member, const std::vector<double>& x) const {
    std::vector<double> vals;
    for (auto j : nn(x))
        for (const auto& [m, v] : pts_[j].second)
            if (m == member) vals.push_back(v);
    if (vals.empty()) return std::nullopt;
    std::sort(vals.begin(), vals.end());
    return std::max(0.0, std::exp(vals[vals.size() / 2]) - EPS);
}

std::optional<std::string> KnnModel::winner(const std::vector<double>& x,
                                            const std::vector<std::string>& row_members) const {
    std::map<std::string, int> votes;
    for (auto j : nn(x)) {
        const auto& t = pts_[j].second;
        auto val = [&](const std::string& m) -> std::optional<double> {
            for (const auto& [n, v] : t)
                if (n == m) return v;
            return std::nullopt;
        };
        std::optional<std::pair<std::pair<double, int>, std::string>> w;
        for (const auto& m : row_members)
            if (auto v = val(m)) {
                auto key = std::make_pair(*v, member_index(m));
                if (!w || key < w->first) w = std::make_pair(key, m);
            }
        if (w) ++votes[w->second];
    }
    if (votes.empty()) return std::nullopt;
    std::optional<std::pair<std::string, int>> best;  // first maximum in name order
    for (const auto& [m, v] : votes)
        if (!best || v > best->second) best = std::make_pair(m, v);
    return best->first;
}

History::History(const std::vector<Row>& train) {
    for (const auto& r : train) {
        auto& hb = b[r.bucket];
        const auto best = fastest(r);
        for (const auto& [m, run] : r.runs) {
            auto& e = hb.try_emplace(m, std::array<double, 3>{0.0, 0.0, 0.0}).first->second;
            e[0] += 1;
            e[1] += run.state == State::Ans ? run.s : r.T;
            e[2] += (best && m == *best) ? 1 : 0;
        }
    }
}

// ------------------------------------------------------------------ policies
namespace {
Est rule_est(const Row& r) {
    Est est;
    for (const auto& [m, run] : r.runs) est[m] = rule_estimate(m, r.x);
    return est;
}
}  // namespace

Plan plan_rules(const Row& r) { return {rule_est(r), std::nullopt, 0.0}; }

PlanFn make_plan_history(std::shared_ptr<History> h) {
    return [h](const Row& r) {
        Est est = rule_est(r);
        static const std::map<std::string, std::array<double, 3>> kEmpty;
        auto bit = h->b.find(r.bucket);
        const auto& hb = bit == h->b.end() ? kEmpty : bit->second;
        std::optional<std::string> lead;
        for (const auto& m : MEMBERS) {
            if (!r.has(m)) continue;
            auto it = hb.find(m);
            if (it != hb.end() && it->second[0] >= 1) {
                est[m] = it->second[1] / it->second[0];
                if (it->second[2] >= 1 && (!lead || est[m] < est[*lead])) lead = m;
            }
        }
        const double delay = lead ? std::min(3.0 * est[*lead] + 0.2, 0.3 * r.T) : 0.0;
        return Plan{est, lead, delay};
    };
}

PlanFn make_plan_model(std::shared_ptr<Model> model, bool head_start) {
    // The portfolio.cpp predict hook: predicted seconds replace the estimates;
    // the fastest predicted member leads by min(3 x its time + 0.2, 30% T).
    return [model, head_start](const Row& r) {
        Est est = rule_est(r);
        std::optional<std::string> best;
        for (const auto& m : MEMBERS) {
            if (!r.has(m)) continue;
            if (auto p = model->seconds(m, r.x)) {
                est[m] = *p;
                if (!best || *p < est[*best]) best = m;
            }
        }
        if (!head_start || !best) return Plan{est, std::nullopt, 0.0};
        return Plan{est, best, std::min(3.0 * est[*best] + 0.2, 0.3 * r.T)};
    };
}

PlanFn make_plan_winner(std::shared_ptr<KnnModel> knn) {
    return [knn](const Row& r) {
        Est est = rule_est(r);
        std::vector<std::string> members;
        for (const auto& [m, run] : r.runs) members.push_back(m);
        auto w = knn->winner(r.x, members);
        if (!w) return Plan{est, std::nullopt, 0.0};
        est[*w] = -1.0;
        return Plan{est, w, std::min(Z3_FIRST_S, 0.1 * r.T)};
    };
}

PlanFn plan_static_first(const std::string& member) {
    // A diagnostic static rule: `member` (when it takes the query) gets the
    // 0.15 s head start Z3 gets today; tells a learned gain from a simpler
    // fixed rule.
    return [member](const Row& r) {
        Est est = rule_est(r);
        if (!r.has(member)) return Plan{est, std::nullopt, 0.0};
        est[member] = -1.0;
        return Plan{est, member, std::min(Z3_FIRST_S, 0.1 * r.T)};
    };
}

Eval evaluate(const std::vector<Row>& rows, const PlanFn& plan, int k) {
    Eval ev;
    int timeouts = 0, first_ok = 0;
    for (const auto& r : rows) {
        auto p = plan(r);
        auto [wall, win] = simulate(r, p.est, p.lead, p.delay, k);
        ev.walls.push_back(wall);
        timeouts += win.empty();
        std::optional<std::string> first = p.lead;
        if (!first) {
            std::optional<std::pair<std::pair<double, int>, std::string>> best;
            for (const auto& [m, run] : r.runs) {
                auto it = p.est.find(m);
                auto key = std::make_pair(it == p.est.end() ? 1.5 : it->second, member_index(m));
                if (!best || key < best->first) best = std::make_pair(key, m);
            }
            if (best) first = best->second;
        }
        const auto f = fastest(r);
        first_ok += (first && f && *first == *f) ? 1 : 0;
    }
    auto s = ev.walls;
    std::sort(s.begin(), s.end());
    const double n = static_cast<double>(std::max<std::size_t>(1, s.size()));
    ev.summary["total_s"] = py_round(sum(ev.walls), 3);
    ev.summary["timeouts"] = timeouts;
    ev.summary["median_s"] = s.empty() ? 0.0 : py_round(s[s.size() / 2], 4);
    ev.summary["p90_s"] =
        s.empty() ? 0.0 : py_round(s[std::min(s.size() - 1, static_cast<std::size_t>(0.9 * static_cast<double>(s.size())))], 4);
    ev.summary["first_is_fastest"] = py_round(first_ok / n, 3);
    return ev;
}

std::pair<double, double> bootstrap_delta(const std::vector<Row>& rows, const std::vector<double>& a,
                                          const std::vector<double>& b, int n, std::uint64_t seed) {
    // 95% interval of (sum b - sum a) / sum a, resampling SOURCE FILES.
    std::map<std::string, std::vector<std::size_t>> by;
    for (std::size_t i = 0; i < rows.size(); ++i) by[rows[i].key].push_back(i);
    std::vector<std::string> files;
    for (const auto& [f, v] : by) files.push_back(f);
    PyRandom rnd(seed);
    std::vector<double> out;
    for (int it = 0; it < n; ++it) {
        double sa = 0.0, sb = 0.0;
        for (std::size_t f = 0; f < files.size(); ++f)
            for (auto i : by[files[rnd.randbelow(files.size())]]) {
                sa += a[i];
                sb += b[i];
            }
        out.push_back(sa != 0.0 ? (sb - sa) / sa : 0.0);
    }
    std::sort(out.begin(), out.end());
    return {out[static_cast<std::size_t>(0.025 * n)], out[static_cast<std::size_t>(0.975 * n)]};
}

Replay run_all(const std::vector<Row>& rows, const std::vector<int>& ks, int n_trees, const std::vector<Row>* repeat) {
    Replay out;
    auto& res = out.res;
    auto [train, test] = split(rows);
    std::set<std::string> trf, tef;
    for (const auto& r : train) trf.insert(r.key);
    for (const auto& r : test) tef.insert(r.key);
    res["queries"] = rows.size();
    res["train"] = train.size();
    res["test"] = test.size();
    res["train_files"] = trf.size();
    res["test_files"] = tef.size();
    res["members"] = ojson::object();
    for (const auto& m : MEMBERS) {
        int c = 0;
        for (const auto& r : rows) c += r.has(m);
        res["members"][m] = c;
    }
    if (train.size() < 20 || test.size() < 10) {
        res["note"] = "not enough data";
        return out;
    }
    auto gbdt = std::make_shared<GbdtModel>(train, false, n_trees);
    auto aft = std::make_shared<GbdtModel>(train, true, n_trees);
    auto knn = std::make_shared<KnnModel>(train);
    auto hist = std::make_shared<History>(train);
    const std::vector<std::pair<std::string, PlanFn>> plans = {
        {"rules", plan_rules},
        {"history", make_plan_history(hist)},
        {"gbdt", make_plan_model(gbdt)},
        {"aft", make_plan_model(aft)},
        {"knn", make_plan_model(knn)},
        {"gbdt-order", make_plan_model(gbdt, false)},
        {"aft-order", make_plan_model(aft, false)},
        {"knn-order", make_plan_model(knn, false)},
        {"winner", make_plan_winner(knn)},
        {"static-bitwuzla", plan_static_first("bitwuzla")}};
    res["k"] = ojson::object();
    for (int k : ks) {
        ojson per = ojson::object();
        std::vector<double> base;
        for (const auto& [name, p] : plans) {
            auto ev = evaluate(test, p, k);
            if (name == "rules") base = ev.walls;
            else {
                auto [lo, hi] = bootstrap_delta(test, base, ev.walls);
                ev.summary["delta_vs_rules_ci95"] = ojson::array({py_round(lo, 4), py_round(hi, 4)});
            }
            per[name] = ev.summary;
        }
        std::vector<double> oracle;
        int otimeouts = 0;
        for (const auto& r : test) {
            auto f = fastest(r);
            oracle.push_back(f ? r.run(*f)->s : r.T);
            otimeouts += !f;
        }
        per["oracle"] = ojson{{"total_s", py_round(sum(oracle), 3)}, {"timeouts", otimeouts}};
        res["k"][std::to_string(k)] = per;
    }
    if (repeat && !repeat->empty()) {
        // Run-to-run noise: the same held-out queries timed twice, replayed
        // with the rule policy.
        std::map<std::string, const Row*> rep;
        for (const auto& r : *repeat)
            if (!r.sha.empty()) rep[r.sha] = &r;
        std::vector<Row> common, again;
        for (const auto& r : test)
            if (rep.count(r.sha)) {
                common.push_back(r);
                again.push_back(*rep[r.sha]);
            }
        res["noise"] = ojson{{"queries", common.size()}};
        for (int k : ks) {
            const double a = evaluate(common, plan_rules, k).summary["total_s"].get<double>();
            const double b = evaluate(again, plan_rules, k).summary["total_s"].get<double>();
            res["noise"][std::to_string(k)] =
                ojson{{"run1_s", a}, {"run2_s", b}, {"rel", py_round(std::abs(a - b) / std::max(a, 1e-9), 4)}};
        }
    }
    out.gbdt_models = {{"gbdt", gbdt}, {"aft", aft}};
    return out;
}

namespace {
std::string ks_repr(const std::vector<int>& ks) {
    std::string s = "[";
    for (std::size_t i = 0; i < ks.size(); ++i) s += (i ? ", " : "") + std::to_string(ks[i]);
    return s + "]";
}
}  // namespace

std::pair<std::optional<std::string>, std::string> decide(const ojson& res, const std::vector<int>& ks,
                                                          double min_margin) {
    // A model is enabled only if, at EVERY k, it beats both baselines by more
    // than max(5%, run-to-run noise) in total time and has no more timeouts.
    if (!res.contains("k")) return {std::nullopt, "not enough data"};
    double noise = 0.0;
    for (int k : ks) {
        const auto key = std::to_string(k);
        if (res.contains("noise") && res["noise"].contains(key) && res["noise"][key].contains("rel"))
            noise = std::max(noise, res["noise"][key]["rel"].get<double>());
    }
    const double margin = std::max(min_margin, noise);
    std::vector<std::string> why;
    for (const char* name : {"gbdt", "aft", "gbdt-order", "aft-order"}) {  // what the C++ loader evaluates
        bool ok = true;
        for (int k : ks) {
            const auto& per = res["k"][std::to_string(k)];
            const double base = std::min(per["rules"]["total_s"].get<double>(), per["history"]["total_s"].get<double>());
            const auto& m = per[name];
            const auto mt = m["timeouts"].get<long long>();
            if (!(m["total_s"].get<double>() < (1 - margin) * base &&
                  mt <= std::min(per["rules"]["timeouts"].get<long long>(), per["history"]["timeouts"].get<long long>()))) {
                ok = false;
                why.push_back(std::string(name) + " k=" + std::to_string(k) + ": " + py(m["total_s"]) + " s / " +
                              py(m["timeouts"]) + " timeouts vs baseline " + py_repr(base) + " s");
                break;
            }
        }
        if (ok) return {std::string(name), "beats the baselines by > " + py_percent(margin, 1) + " at k in " + ks_repr(ks)};
    }
    std::string msg = "no model beats the baselines by > " + py_percent(margin, 1) + ": ";
    for (std::size_t i = 0; i < why.size(); ++i) msg += (i ? "; " : "") + why[i];
    return {std::nullopt, msg};
}

std::string markdown(const ojson& res) {
    std::vector<std::string> out;
    std::string members;
    if (res.contains("members"))
        for (auto it = res["members"].begin(); it != res["members"].end(); ++it)
            members += (members.empty() ? "" : ", ") + it.key() + " " + py(it.value());
    auto get = [&](const char* k) { return res.contains(k) ? py(res[k]) : std::string("None"); };
    out.push_back(get("queries") + " distinct VCs (" + get("train") + " train from " + get("train_files") +
                  " files, " + get("test") + " held out from " + get("test_files") + " files); members: " + members);
    out.push_back("");
    if (res.contains("k"))
        for (auto it = res["k"].begin(); it != res["k"].end(); ++it) {
            out.push_back("k = " + it.key() + " cores");
            out.push_back("");
            out.push_back("| policy | total s | timeouts | median s | p90 s | first pick fastest | total vs rules (95% CI) |");
            out.push_back("|---|---:|---:|---:|---:|---:|---|");
            for (auto pv = it.value().begin(); pv != it.value().end(); ++pv) {
                const auto& v = pv.value();
                std::string cis;
                if (v.contains("delta_vs_rules_ci95") && !v["delta_vs_rules_ci95"].empty()) {
                    const auto& ci = v["delta_vs_rules_ci95"];
                    cis = py_signed_fixed(ci[0].get<double>() * 100, 1) + "% .. " +
                          py_signed_fixed(ci[1].get<double>() * 100, 1) + "%";
                }
                auto opt = [&](const char* k) { return v.contains(k) ? py(v[k]) : std::string(); };
                out.push_back("| " + pv.key() + " | " + py(v["total_s"]) + " | " + py(v["timeouts"]) + " | " +
                              opt("median_s") + " | " + opt("p90_s") + " | " + opt("first_is_fastest") + " | " + cis +
                              " |");
            }
            out.push_back("");
        }
    if (res.contains("noise")) {
        const auto& nz = res["noise"];
        std::string parts;
        for (auto it = nz.begin(); it != nz.end(); ++it) {
            if (it.key() == "queries") continue;
            const auto& v = it.value();
            parts += (parts.empty() ? "" : ", ") + std::string("k=") + it.key() + ": " + py(v["run1_s"]) + " s vs " +
                     py(v["run2_s"]) + " s (" + py_fixed(v["rel"].get<double>() * 100, 1) + "%)";
        }
        out.push_back("run-to-run noise (rules, " + py(nz["queries"]) + " held-out VCs timed twice): " + parts);
        out.push_back("");
    }
    std::string s;
    for (std::size_t i = 0; i < out.size(); ++i) s += (i ? "\n" : "") + out[i];
    return s + "\n";
}

int replay_main(int argc, char** argv, std::ostream& out, std::ostream& err) {
    std::vector<fs::path> logs, repeat;
    std::vector<int> ks = predict::REPLAY_KS;
    int trees = 60;
    std::optional<fs::path> json_out;
    for (int i = 0; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--solve-log") logs.emplace_back(next());
        else if (a == "--repeat-solve-log") repeat.emplace_back(next());
        else if (a == "--trees") trees = std::stoi(next());
        else if (a == "--json") json_out = next();
        else if (a == "--ks") {
            ks.clear();
            std::stringstream ss(next());
            for (std::string t; std::getline(ss, t, ',');) ks.push_back(std::stoi(t));
        } else {
            err << "usage: prism_ai replay --solve-log FILE [--repeat-solve-log FILE] [--ks 1,2,3,5] [--trees N] "
                   "[--json OUT]\n";
            return 2;
        }
    }
    if (logs.empty()) {
        err << "prism_ai replay: --solve-log is required\n";
        return 2;
    }
    auto rows = load(logs);
    std::vector<Row> rep;
    if (!repeat.empty()) rep = load(repeat);
    auto r = run_all(rows, ks, trees, repeat.empty() ? nullptr : &rep);
    std::vector<int> dks;
    for (int k : predict::DECIDE_KS)
        if (std::find(ks.begin(), ks.end(), k) != ks.end()) dks.push_back(k);
    auto [chosen, why] = decide(r.res, dks.empty() ? ks : dks);
    r.res["chosen"] = chosen ? ojson(*chosen) : ojson(nullptr);
    r.res["why"] = why;
    if (json_out) std::ofstream(*json_out, std::ios::binary) << r.res.dump(1) << "\n";
    out << markdown(r.res) << "replay decision: " << (chosen ? *chosen : "none") << " (" << why << ")\n";
    return 0;
}

}  // namespace sched
}  // namespace prism_ai
