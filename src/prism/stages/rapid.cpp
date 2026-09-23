// Stages rapid and muttest: property trials against comment contracts and
// mutation testing of those contracts.
#include "interp.hpp"

namespace prism {
namespace fs = std::filesystem;
using namespace stages_detail;

namespace {
Spec spec_comments_rapid(const FunctionInfo& fn) {
    Spec spec;
    auto text = read_fn_source(fn);
    if (text.empty()) return spec;
    std::vector<std::string> lines;
    {
        std::istringstream ss(text);
        std::string ln;
        while (std::getline(ss, ln)) lines.push_back(ln);
    }
    int start = std::max(0, (fn.line ? fn.line : 1) - 2);
    int end = (fn.span.second ? fn.span.second : (fn.line ? fn.line : 1) + 20);
    end = std::min(static_cast<int>(lines.size()), end);
    static Regex clause("(?://|/\\*|\\*)\\s*(requires|ensures|invariant|decreases|diff)\\s*:\\s*(.+?)(?:\\*/)?\\s*$");
    for (int i = start; i < end; ++i) {
        auto m = clause.search_match(lines[static_cast<std::size_t>(i)]);
        if (!m) continue;
        auto kind = lower_copy(m->group(1));
        auto val = strip(m->group(2));
        if (val.ends_with("*/")) val = strip(val.substr(0, val.size() - 2));
        if (kind == "diff") {
            auto sp = val.find(' ');
            spec.diff = sp == std::string::npos ? val : val.substr(0, sp);
        } else if (kind == "requires") spec.requires_.push_back(val);
        else if (kind == "ensures") spec.ensures.push_back(val);
        else if (kind == "invariant") spec.invariant.push_back(val);
        else if (kind == "decreases") spec.decreases.push_back(val);
    }
    if (spec.ensures.empty() && spec.requires_.empty()) {
        auto acsl = parse_comments(fn);
        if (!acsl.ensures.empty() || !acsl.requires_.empty()) return acsl;
    }
    return spec;
}

constexpr int DEFAULT_LO = -256;

constexpr int DEFAULT_HI = 256;

struct Bound {
    int lo = DEFAULT_LO;
    int hi = DEFAULT_HI;
    std::set<int> neq;
};

bool unsigned_typ(std::string t) {
    t = lower_copy(t);
    for (char& c : t)
        if (c == '*') c = ' ';
    t = strip(t);
    return t.find("unsigned") != std::string::npos || t.starts_with("uint") || t == "size_t" || t == "_bool" ||
           t == "bool";
}

void apply_atom(std::map<std::string, Bound>& bounds, const std::string& atom) {
    static Regex req("^([A-Za-z_]\\w*)\\s*(<|>=|!=)\\s*(0|[1-9]\\d*)$");
    static Regex extra("^([A-Za-z_]\\w*)\\s*(<=|>|==)\\s*(0|[1-9]\\d*|-?[1-9]\\d*)$");
    auto s = strip(atom);
    auto m = match_at(req, s);
    if (!m) m = match_at(extra, s);
    if (!m) return;
    auto name = m->group(1);
    auto op = m->group(2);
    int raw = std::stoi(m->group(3));
    if (!bounds.contains(name)) bounds[name] = {};
    auto& b = bounds[name];
    if (op == "<") b.hi = std::min(b.hi, raw - 1);
    else if (op == "<=") b.hi = std::min(b.hi, raw);
    else if (op == ">") b.lo = std::max(b.lo, raw + 1);
    else if (op == ">=") b.lo = std::max(b.lo, raw);
    else if (op == "!=") b.neq.insert(raw);
    else if (op == "==") {
        b.lo = std::max(b.lo, raw);
        b.hi = std::min(b.hi, raw);
    }
}

int pick_bound(std::mt19937& rng, const Bound& b, const std::vector<int>* interesting) {
    if (interesting) {
        std::vector<int> cand;
        for (int v : *interesting)
            if (v >= b.lo && v <= b.hi && !b.neq.contains(v)) cand.push_back(v);
        if (!cand.empty()) {
            std::uniform_int_distribution<int> d(0, static_cast<int>(cand.size()) - 1);
            return cand[static_cast<std::size_t>(d(rng))];
        }
    }
    for (int i = 0; i < 64; ++i) {
        std::uniform_int_distribution<int> d(b.lo, b.hi);
        int v = d(rng);
        if (!b.neq.contains(v)) return v;
    }
    for (int v = b.lo; v <= b.hi; ++v)
        if (!b.neq.contains(v)) return v;
    throw std::runtime_error("empty domain");
}

struct RapidPlan {
    Spec spec;
    std::vector<std::string> requires_;
    std::vector<std::string> ensures;
    std::vector<Args> samples;
    std::map<std::string, Bound> bounds;
    std::string error;
    int trials = 0;
};

std::optional<RapidPlan> plan_trials(const FunctionInfo& fn, int trials) {
    if (fn.kind != "SCALAR") return std::nullopt;
    auto spec = spec_comments_rapid(fn);
    if (spec.ensures.empty()) return std::nullopt;
    RapidPlan plan;
    plan.spec = spec;
    plan.requires_ = spec.requires_;
    plan.ensures = spec.ensures;
    plan.trials = trials;
    try {
        std::map<std::string, Bound> bounds;
        for (auto& [typ, name] : fn.params) {
            if (name.empty()) continue;
            Bound b;
            if (unsigned_typ(typ)) b.lo = 0;
            bounds[name] = b;
        }
        for (auto& atom : plan.requires_) apply_atom(bounds, atom);
        for (auto& [name, b] : bounds) {
            if (b.lo > b.hi) throw std::runtime_error("empty domain for " + name + " after requires");
            int viable = b.hi - b.lo + 1;
            for (int x : b.neq)
                if (x >= b.lo && x <= b.hi) --viable;
            if (viable <= 0) throw std::runtime_error("empty domain for " + name + " after requires");
        }
        std::string seed_s = fn.file + ":" + fn.name + ":" + std::to_string(trials);
        uint32_t seed = 2166136261u;
        for (unsigned char c : seed_s) seed = (seed ^ c) * 16777619u;
        std::mt19937 rng(seed);
        std::vector<int> interesting{0, 1, -1, 2, -2, 42, 99, 100, 127, -128, 255, -256};
        std::vector<std::string> names;
        for (auto& [t, n] : fn.params)
            if (!n.empty()) names.push_back(n);
        for (int i = 0; i < std::max(0, trials); ++i) {
            Args env;
            for (auto& name : names)
                env[name] = pick_bound(rng, bounds[name], i < 12 ? &interesting : nullptr);
            plan.samples.push_back(env);
        }
        plan.bounds = std::move(bounds);
    } catch (const std::exception& ex) {
        plan.error = ex.what();
    }
    return plan;
}

std::optional<std::string> failing_ensures(const std::vector<std::string>& ensures, const Args& env, int result) {
    auto full = env;
    full["result"] = result;
    static Regex ens("^result\\s*(==|>=)\\s*(.+)$");
    for (auto& clause : ensures) {
        auto s = strip(clause);
        if (auto m = match_at(ens, s)) {
            auto op = m->group(1);
            auto rhs = m->group(2);
            try {
                St st({}, {}, {});
                for (auto& [k, v] : full) st.vars[k] = v;
                auto rv = static_cast<int>(eval_src(st, rhs));
                if (op == "==" && result != rv) return clause;
                if (op == ">=" && !(result >= rv)) return clause;
                continue;
            } catch (...) {
                try {
                    St st({}, {}, {});
                    for (auto& [k, v] : full) st.vars[k] = v;
                    if (!truth(eval_src(st, s))) return clause;
                    continue;
                } catch (...) {
                }
            }
        }
        try {
            St st({}, {}, {});
            for (auto& [k, v] : full) st.vars[k] = v;
            if (!truth(eval_src(st, s))) return clause;
        } catch (...) {
            return clause;
        }
    }
    return std::nullopt;
}

bool env_in_bounds(const Args& env, const std::map<std::string, Bound>& bounds) {
    for (auto& [name, v] : env) {
        auto it = bounds.find(name);
        if (it == bounds.end()) continue;
        auto& b = it->second;
        if (v < b.lo || v > b.hi || b.neq.contains(v)) return false;
    }
    return true;
}

std::vector<int> shrink_cands(int v) {
    std::vector<int> out;
    if (v != 0) out.push_back(0);
    if (v > 1 || v < -1) out.push_back(v / 2);
    if (v > 0) out.push_back(v - 1);
    else if (v < 0) out.push_back(v + 1);
    std::vector<int> uniq;
    std::set<int> seen;
    for (int x : out) {
        if (seen.insert(x).second) uniq.push_back(x);
    }
    return uniq;
}

std::pair<Args, int> shrink_counterexample(const FunctionInfo& fn, Args env,
                                           const std::vector<std::string>& ensures,
                                           const std::map<std::string, Bound>& bounds, bool ub) {
    auto fails = [&](const Args& cand) {
        auto rec = execute(fn, cand);
        if (!rec.error.empty()) return false;
        if (!rec.ub.empty()) return true;
        if (ub) return false;
        int result = rec.value ? static_cast<int>(*rec.value) : 0;
        return failing_ensures(ensures, cand, result).has_value();
    };
    int shrinks = 0;
    bool progress = true;
    while (progress && shrinks < 64) {
        progress = false;
        for (auto& [k, v] : env) {
            for (int c : shrink_cands(v)) {
                auto trial = env;
                trial[k] = c;
                if (trial == env || !env_in_bounds(trial, bounds)) continue;
                if (fails(trial)) {
                    env = std::move(trial);
                    ++shrinks;
                    progress = true;
                    break;
                }
            }
            if (progress) break;
        }
    }
    return {env, shrinks};
}

struct PlanRun {
    bool ok = false;
    std::string error;
    std::string counterexample;
    std::string engine;
    std::string clause;
    std::optional<int> result;
    Args env;
    int n = 0;
    int shrinks = 0;
};

PlanRun run_plan(const FunctionInfo& fn, const RapidPlan& plan) {
    if (!plan.error.empty()) return {false, plan.error, "", "", "", std::nullopt, {}, 0};
    std::vector<std::optional<int>> results;
    std::string ub_err;
    try {
        for (auto& env : plan.samples) {
            auto rec = execute(fn, env);
            if (!rec.error.empty()) {
                ub_err = rec.error;
                break;
            }
            if (!rec.ub.empty()) {
                results.emplace_back(std::nullopt);
                continue;
            }
            results.push_back(rec.value ? static_cast<int>(*rec.value) : 0);
        }
        if (ub_err.empty() && results.size() == plan.samples.size()) {
            for (std::size_t i = 0; i < plan.samples.size(); ++i) {
                if (!results[i]) {
                    auto [env, nsh] = shrink_counterexample(fn, plan.samples[i], plan.ensures, plan.bounds, true);
                    std::string cex;
                    for (auto& [k, v] : env) {
                        if (!cex.empty()) cex += ", ";
                        cex += k + "=" + std::to_string(v);
                    }
                    return {false, "", cex + " -> undefined-behavior", "concrete", "undefined-behavior",
                            std::nullopt, env, 0, nsh};
                }
                auto failed = failing_ensures(plan.ensures, plan.samples[i], *results[i]);
                if (failed) {
                    auto [env, nsh] = shrink_counterexample(fn, plan.samples[i], plan.ensures, plan.bounds, false);
                    auto rec2 = execute(fn, env);
                    std::optional<int> result = results[i];
                    if (rec2.error.empty() && rec2.ub.empty() && rec2.value)
                        result = static_cast<int>(*rec2.value);
                    if (result) {
                        auto again = failing_ensures(plan.ensures, env, *result);
                        if (again) failed = again;
                    }
                    std::string cex;
                    for (auto& [k, v] : env) {
                        if (!cex.empty()) cex += ", ";
                        cex += k + "=" + std::to_string(v);
                    }
                    return {false, "", cex + " -> result=" + std::to_string(result.value_or(0)), "concrete", *failed,
                            result, env, 0, nsh};
                }
            }
            return {true, "", "", "concrete", "", std::nullopt, {}, static_cast<int>(plan.samples.size())};
        }
    } catch (...) {
    }
    return {false, ub_err.empty() ? "cannot evaluate " + fn.name : ub_err, "", "interp", "", std::nullopt, {},
            0};
}

Finding finding_from_plan(const FunctionInfo& fn, const RapidPlan& plan, const std::string& stage) {
    auto extra_req = nlohmann::json(plan.requires_).dump();
    auto extra_ens = nlohmann::json(plan.ensures).dump();
    auto fill = [&](Finding f) {
        f.extra["requires"] = extra_req;
        f.extra["ensures"] = extra_ens;
        f.extra["trials"] = std::to_string(plan.trials);
        f.extra["sampled"] = "true";
        return f;
    };
    auto compiler_missing = [](const std::string& err) {
        auto text = lower_copy(err);
        if (text.find("no gcc") != std::string::npos || text.find("gcc/clang") != std::string::npos)
            return true;
        if (text.find("not on path") != std::string::npos &&
            (text.find("gcc") != std::string::npos || text.find("clang") != std::string::npos ||
             text.find("compiler") != std::string::npos))
            return true;
        Config cfg;
        return !cfg.which({"gcc", "clang"}).has_value();
    };
    if (!plan.error.empty() && plan.samples.empty()) {
        bool missing = compiler_missing(plan.error);
        auto f = fill(make_find(stage, missing ? laws::NOTRUN : laws::ERROR, fn, "", plan.error,
                                laws::STRENGTH_SOME));
        if (missing) f.extra["install"] = "install gcc or clang";
        return f;
    }
    auto info = run_plan(fn, plan);
    if (!info.error.empty() && info.counterexample.empty()) {
        bool missing = compiler_missing(info.error);
        auto f = fill(make_find(stage, missing ? laws::NOTRUN : laws::ERROR, fn, "", info.error,
                                laws::STRENGTH_SOME));
        if (missing) f.extra["install"] = "install gcc or clang";
        return f;
    }
    if (!info.ok) {
        auto clause = info.clause.empty() ? join_sv(plan.ensures, " && ") : info.clause;
        auto f = make_find(stage, laws::FAILED, fn, "FUNC-CONTRACT",
                           "ensures (" + clause + ") failed on " + info.counterexample, laws::STRENGTH_FINDS);
        f.evidence = fn.body.substr(0, 400);
        f.counterexample = info.counterexample;
        f = fill(std::move(f));
        f.extra["engine"] = info.engine;
        // extra.shrinks counts cex minimization; FAILED is a cex, never a proof.
        f.extra["shrinks"] = std::to_string(info.shrinks);
        return f;
    }
    auto ens = join_sv(plan.ensures, " && ");
    int n = info.n ? info.n : static_cast<int>(plan.samples.size());
    auto f = make_find(stage, laws::CLEAN, fn, "",
                       "all " + std::to_string(n) + " trials hold for ensures (" + ens +
                           "); not a proof - sampling is not forall",
                       laws::STRENGTH_SOME);
    f = fill(std::move(f));
    f.extra["engine"] = info.engine;
    return f;
}

std::vector<std::tuple<int, int, std::string, std::string>> iter_mutations(const std::string& body) {
    std::vector<std::tuple<int, int, std::string, std::string>> sites;
    std::size_t i = 0;
    char quote = 0;
    while (i < body.size()) {
        char c = body[i];
        if (quote) {
            if (c == '\\' && i + 1 < body.size()) {
                i += 2;
                continue;
            }
            if (c == quote) quote = 0;
            ++i;
            continue;
        }
        if (c == '"' || c == '\'') {
            quote = c;
            ++i;
            continue;
        }
        if (i + 2 <= body.size() && body.compare(i, 2, "==") == 0) {
            char prev = i ? body[i - 1] : '\0';
            if (prev != '=' && prev != '!') sites.emplace_back(static_cast<int>(i), static_cast<int>(i + 2), "==", "!=");
            i += 2;
            continue;
        }
        if (c == '+') {
            char nxt = i + 1 < body.size() ? body[i + 1] : '\0';
            char prev = i ? body[i - 1] : '\0';
            if (nxt != '+' && nxt != '=' && prev != '+')
                sites.emplace_back(static_cast<int>(i), static_cast<int>(i + 1), "+", "-");
            ++i;
            continue;
        }
        if (c == '<') {
            char nxt = i + 1 < body.size() ? body[i + 1] : '\0';
            char prev = i ? body[i - 1] : '\0';
            if (nxt != '<' && nxt != '=' && prev != '<')
                sites.emplace_back(static_cast<int>(i), static_cast<int>(i + 1), "<", ">");
            ++i;
            continue;
        }
        ++i;
    }
    return sites;
}

}  // namespace

std::optional<Finding> contract_kind_finding(const FunctionInfo& fn, const std::string& stage) {
    if (fn.kind == "SCALAR") return std::nullopt;
    auto spec = spec_comments_rapid(fn);
    if (spec.ensures.empty()) return std::nullopt;
    auto f = make_find(stage, laws::NEEDS_HARNESS, fn, "",
                       fn.kind + ": RapidCheck would invent a buffer or pass NULL; not a sampled proof",
                       laws::STRENGTH_SOME);
    f.extra["requires"] = nlohmann::json(spec.requires_).dump();
    f.extra["ensures"] = nlohmann::json(spec.ensures).dump();
    return f;
}

std::vector<Finding> run_rapid(const std::vector<FunctionInfo>& functions, int trials) {
    std::vector<Finding> out;
    for (auto& fn : functions) {
        if (auto kind = contract_kind_finding(fn, "rapid")) {
            out.push_back(*kind);
            continue;
        }
        auto plan = plan_trials(fn, trials);
        if (!plan) continue;
        out.push_back(finding_from_plan(fn, *plan, "rapid"));
    }
    return out;
}

std::vector<Finding> run_muttest(const std::vector<FunctionInfo>& functions, int trials) {
    // Killing a mutant is CLEAN (not a proof). Missing gcc/clang is NOTRUN, never CLEAN.
    // Eval errors without a cex are ERROR/NOTRUN, not a killed mutant.
    auto compiler_missing = [](const std::string& err) {
        auto text = lower_copy(err);
        if (text.find("no gcc") != std::string::npos || text.find("gcc/clang") != std::string::npos)
            return true;
        if (text.find("not on path") != std::string::npos &&
            (text.find("gcc") != std::string::npos || text.find("clang") != std::string::npos ||
             text.find("compiler") != std::string::npos))
            return true;
        Config cfg;
        return !cfg.which({"gcc", "clang"}).has_value();
    };
    std::vector<Finding> out;
    for (auto& fn : functions) {
        if (auto kind = contract_kind_finding(fn, "muttest")) {
            out.push_back(*kind);
            continue;
        }
        auto plan = plan_trials(fn, trials);
        if (!plan) continue;
        auto sites = iter_mutations(fn.body);
        if (sites.empty()) continue;
        auto baseline = run_plan(fn, *plan);
        if (!baseline.error.empty() && baseline.counterexample.empty()) {
            bool missing = compiler_missing(baseline.error);
            auto f = make_find("muttest", missing ? laws::NOTRUN : laws::ERROR, fn, "",
                               "cannot score mutants: " + baseline.error, laws::STRENGTH_SOME);
            if (missing) f.extra["install"] = "install gcc or clang";
            out.push_back(std::move(f));
            continue;
        }
        for (auto& [start, end, src, dst] : sites) {
            auto mutated = fn.body;
            mutated.replace(static_cast<std::size_t>(start), static_cast<std::size_t>(end - start), dst);
            auto cloned = fn;
            cloned.body = mutated;
            auto info = run_plan(cloned, *plan);
            std::map<std::string, std::string> extra{
                {"from", src},
                {"to", dst},
                {"mutation", src + " -> " + dst},
                {"offset", std::to_string(start)},
                {"trials", std::to_string(trials)},
                {"engine", info.engine},
                {"requires", nlohmann::json(plan->requires_).dump()},
                {"ensures", nlohmann::json(plan->ensures).dump()},
            };
            if (!info.error.empty() && info.counterexample.empty()) {
                bool missing = compiler_missing(info.error);
                if (missing) extra["install"] = "install gcc or clang";
                auto f = make_find("muttest", missing ? laws::NOTRUN : laws::ERROR, fn, "",
                                   "cannot score mutant " + src + " -> " + dst + ": " + info.error,
                                   laws::STRENGTH_SOME);
                f.extra = extra;
                out.push_back(std::move(f));
                continue;
            }
            bool killed = !info.ok;
            if (killed) {
                auto f = make_find("muttest", laws::CLEAN, fn, "",
                                   "mutant killed: " + src + " -> " + dst + " (" +
                                       (info.counterexample.empty() ? "tests failed on mutant"
                                                                    : info.counterexample) +
                                       "); silence is not a proof",
                                   laws::STRENGTH_SOME);
                f.extra = extra;
                out.push_back(std::move(f));
            } else {
                auto f = make_find("muttest", laws::FAILED, fn, "FUNC-CONTRACT",
                                   "mutant survived: " + src + " -> " + dst + " (all " +
                                       std::to_string(plan->samples.size()) + " trials still hold; not a proof)",
                                   laws::STRENGTH_FINDS);
                f.evidence = mutated.substr(0, 400);
                f.extra = extra;
                out.push_back(std::move(f));
            }
        }
    }
    return out;
}

}  // namespace prism
