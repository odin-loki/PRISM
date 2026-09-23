// Stage concolic: concrete runs plus branch flips (heuristic or Z3).
#include "interp.hpp"

namespace prism {
namespace fs = std::filesystem;
using namespace stages_detail;

namespace {
#include "../bmc_unenc.inc"

const std::unordered_set<std::string> kCallKw = {
    "if", "for", "while", "switch", "return", "sizeof", "typeof", "__typeof__", "else", "do",
    "case", "default", "_Generic", "break", "continue", "goto", "struct", "union", "enum",
    "assert", "static_assert", "_Static_assert", "alignof", "_Alignof", "__attribute__",
    "offsetof",
};

bool has_self_call(const FunctionInfo& fn) {
    if (fn.name.empty()) return false;
    static Regex re("\\b([A-Za-z_]\\w*)\\s*\\(");
    for (auto& m : re.finditer(fn.body)) {
        auto n = m.group(1);
        if (!kCallKw.contains(n) && n == fn.name) return true;
    }
    return false;
}

bool has_unencoded_cxx(const FunctionInfo& fn) {
    return re_search("\\bstd::|\\bstring_view\\b|\\bspan\\b", fn.return_type + " " + fn.body);
}

bool has_unencoded_float(const FunctionInfo& fn) {
    static Regex fl("(?i)\\bfloat\\b|\\bdouble\\b");
    if (fl.search(fn.return_type)) return true;
    for (auto& [t, n] : fn.params)
        if (fl.search(t)) return true;
    return re_search("\\d+\\.\\d+[fFlL]?|\\b(?:float|double)\\b", fn.body);
}

bool has_unencoded_throw(const FunctionInfo& fn) { return re_search("\\bthrow\\b", fn.body); }

bool has_unencoded_setjmp(const FunctionInfo& fn) {
    return re_search("\\b(?:setjmp|longjmp|va_list|va_start)\\b", fn.body);
}

using ArgsKey = std::vector<std::pair<std::string, int>>;

ArgsKey args_key(const Args& a) {
    ArgsKey k;
    for (auto& [x, y] : a) k.emplace_back(x, i32(y));
    std::sort(k.begin(), k.end());
    return k;
}

std::vector<Args> initial_seeds(const FunctionInfo& fn) {
    std::vector<std::string> names;
    for (auto& [t, n] : fn.params)
        if (!n.empty()) names.push_back(n);
    if (names.empty()) return {Args{}};
    std::vector<Args> out;
    std::set<ArgsKey> seen;
    auto add = [&](Args args) {
        Args norm;
        for (auto& n : names) norm[n] = i32(args.contains(n) ? args[n] : 0);
        auto key = args_key(norm);
        if (seen.insert(key).second) out.push_back(std::move(norm));
    };
    Args z;
    for (auto& n : names) z[n] = 0;
    add(z);
    for (auto& blob : interesting_seeds(fn)) add(decode_args(fn, blob));
    const int64_t extremes[] = {0, 1, -1, INT_MAX_32, INT_MIN_32};
    for (auto& name : names) {
        for (auto v : extremes) {
            auto nxt = z;
            nxt[name] = i32(v);
            add(nxt);
        }
    }
    if (names.size() <= 3) {
        std::function<void(std::size_t, Args)> rec = [&](std::size_t i, Args cur) {
            if (i == names.size()) {
                add(cur);
                return;
            }
            for (auto v : extremes) {
                cur[names[i]] = i32(v);
                rec(i + 1, cur);
            }
        };
        rec(0, {});
    }
    return out;
}

std::vector<std::string> branch_conditions(const FunctionInfo& fn) {
    static Regex re("\\bif\\s*\\(([^)]+)\\)");
    std::vector<std::string> seen;
    for (auto& m : re.finditer(fn.body)) {
        auto cond = m.group(1);
        std::string n;
        bool sp = false;
        for (char c : cond) {
            if (std::isspace(static_cast<unsigned char>(c))) {
                if (!sp && !n.empty()) {
                    n.push_back(' ');
                    sp = true;
                }
            } else {
                n.push_back(c);
                sp = false;
            }
        }
        while (!n.empty() && n.back() == ' ') n.pop_back();
        if (!n.empty() && std::find(seen.begin(), seen.end(), n) == seen.end()) seen.push_back(n);
    }
    return seen;
}

std::optional<Args> heuristic_flip(const FunctionInfo& fn, const Args& args, const std::string& cond0, bool want) {
    std::unordered_set<std::string> param_names;
    for (auto& [t, n] : fn.params)
        if (!n.empty()) param_names.insert(n);
    auto cond = cond0;
    {
        std::string n;
        bool sp = false;
        for (char c : cond) {
            if (std::isspace(static_cast<unsigned char>(c))) {
                if (!sp && !n.empty()) {
                    n.push_back(' ');
                    sp = true;
                }
            } else {
                n.push_back(c);
                sp = false;
            }
        }
        cond = strip(n);
    }
    static Regex m1("^(\\w+)\\s*(>=|<=|==|!=|>|<)\\s*(-?\\d+)$");
    if (auto m = match_at(m1, cond)) {
        auto var = m->group(1);
        auto op = m->group(2);
        int val = i32(std::stoi(m->group(3)));
        if (!param_names.contains(var)) return std::nullopt;
        auto nxt = args;
        if (op == ">") nxt[var] = want ? val + 1 : val;
        else if (op == ">=") nxt[var] = want ? val : val - 1;
        else if (op == "<") nxt[var] = want ? val - 1 : val;
        else if (op == "<=") nxt[var] = want ? val : val + 1;
        else if (op == "==") nxt[var] = want ? val : val + 1;
        else if (op == "!=") nxt[var] = want ? val + 1 : val;
        else return std::nullopt;
        nxt[var] = i32(nxt[var]);
        auto got = eval_cond(fn, nxt, cond);
        if (got && *got == want) return nxt;
        return std::nullopt;
    }
    static Regex m2("^(\\w+)\\s*(>=|<=|==|!=|>|<)\\s*(\\w+)$");
    if (auto m = match_at(m2, cond)) {
        auto left = m->group(1), op = m->group(2), right = m->group(3);
        if (!param_names.contains(left)) return std::nullopt;
        auto nxt = args;
        int rhs = 0;
        if (param_names.contains(right)) rhs = i32(args.contains(right) ? args.at(right) : 0);
        else {
            try {
                rhs = i32(std::stoi(right, nullptr, 0));
            } catch (...) {
                return std::nullopt;
            }
        }
        if (op == "==") nxt[left] = want ? rhs : rhs + 1;
        else if (op == "!=") nxt[left] = want ? rhs + 1 : rhs;
        else if (op == "<") nxt[left] = want ? rhs - 1 : rhs;
        else if (op == ">") nxt[left] = want ? rhs + 1 : rhs;
        else if (op == "<=") nxt[left] = want ? rhs : rhs + 1;
        else if (op == ">=") nxt[left] = want ? rhs : rhs - 1;
        else return std::nullopt;
        nxt[left] = i32(nxt[left]);
        auto got = eval_cond(fn, nxt, cond);
        if (got && *got == want) return nxt;
        return std::nullopt;
    }
    static Regex m3("^(\\w+)$");
    if (auto m = match_at(m3, cond)) {
        auto var = m->group(1);
        if (!param_names.contains(var)) return std::nullopt;
        auto nxt = args;
        nxt[var] = want ? 1 : 0;
        auto got = eval_cond(fn, nxt, cond);
        if (got && *got == want) return nxt;
    }
    for (auto& name : param_names) {
        auto nxt = args;
        int v = i32(args.contains(name) ? args.at(name) : 0);
        for (int64_t cand : {int64_t{0}, int64_t{1}, int64_t{-1}, int64_t{INT_MAX_32},
                             int64_t{INT_MIN_32}, -int64_t{v}, int64_t{v} + 1, int64_t{v} - 1}) {
            nxt[name] = i32(cand);
            auto got = eval_cond(fn, nxt, cond);
            if (got && *got == want && args_key(nxt) != args_key(args)) return nxt;
        }
    }
    return std::nullopt;
}

std::string oracle_tag(bool this_from_z3, int z3_seeds) {
    if (this_from_z3 || z3_seeds > 0) return "z3";
    return "concrete";
}

// Returns (next_args, via_z3, was_unsat). KLEE Executor::fork drops an
// unsat side; we do the same and do not invent a heuristic neighbor.
std::tuple<std::optional<Args>, bool, bool> neighbor_for_cond(const FunctionInfo& fn, const Args& args,
                                                             const std::string& cond) {
    auto cur = eval_cond(fn, args, cond);
    if (!cur) return {std::nullopt, false, false};
    bool want = !*cur;
    auto solved = solve_fork_flip(fn, args, cond, want);
    if (solved.kind == ForkFlipKind::Unsat) return {std::nullopt, false, true};
    if (solved.kind == ForkFlipKind::Model) return {solved.args, true, false};
    auto nxt = heuristic_flip(fn, args, cond, want);
    return {nxt, false, false};
}

Finding concolic_function(const FunctionInfo& fn, int budget) {
    auto base_nh = [&](std::string msg) {
        return make_find("concolic", laws::NEEDS_HARNESS, fn, "", std::move(msg), laws::STRENGTH_FINDS);
    };
    if (fn.kind == "POINTER")
        return base_nh("pointer parameter: concolic engine does not invent buffers");
    if (fn.kind == "OTHER")
        return base_nh("non-scalar parameter: concolic engine does not invent objects");
    if (body_needs_pointer_harness(fn.body))
        return base_nh("local pointer or heap object: concolic engine does not invent buffers");
    if (has_self_call(fn))
        return base_nh("recursive call unencoded: concrete bound is not a proof of the callee");
    if (has_unencoded_float(fn))
        return base_nh("float/double unencoded: concrete oracle is not an IEEE model");
    if (has_unencoded_cxx(fn))
        return base_nh("C++ view/span unencoded: concolic engine is not a lifetime model");
    if (has_unencoded_throw(fn))
        return base_nh("C++ throw unencoded: concolic engine is not an exception model");
    if (has_unencoded_setjmp(fn))
        return base_nh(
            "setjmp/longjmp/va_list unencoded: concolic engine is not a nonlocal-control model");
    if (auto syn = unencoded_syntax_reason(fn, "concolic engine")) return base_nh(*syn);
    auto queue = initial_seeds(fn);
    std::set<ArgsKey> seen;
    std::set<ArgsKey> z3_keys;
    int tried = 0, generated = 0, z3_seeds = 0, skipped_unsat = 0, max_new = std::max(0, budget);
    auto fill_extra = [&](Finding& f, const Args* crash_args = nullptr, bool this_from_z3 = false) {
        f.extra["tried"] = std::to_string(tried);
        f.extra["generated"] = std::to_string(generated);
        f.extra["oracle"] = oracle_tag(this_from_z3, z3_seeds);
        f.extra["z3_seeds"] = std::to_string(z3_seeds);
        f.extra["skipped_unsat"] = std::to_string(skipped_unsat);
        if (crash_args) {
            std::string argstr;
            for (auto& [k, v] : *crash_args) {
                if (!argstr.empty()) argstr += ", ";
                argstr += k + "=" + std::to_string(v);
            }
            f.extra["args"] = argstr;
        }
    };
    while (!queue.empty()) {
        auto args = queue.front();
        queue.erase(queue.begin());
        auto key = args_key(args);
        if (seen.contains(key)) continue;
        seen.insert(key);
        ++tried;
        auto rec = execute(fn, args);
        if (!rec.error.empty()) {
            if (rec.error == "skip-pointer")
                return base_nh("pointer parameter: concolic engine does not invent buffers");
            if (auto msg = harness_for_parsefail(rec.error, "concolic engine")) {
                auto f = base_nh(*msg);
                f.extra["tried"] = std::to_string(tried);
                return f;
            }
            auto f = make_find("concolic", laws::ERROR, fn, "", rec.error, laws::STRENGTH_FINDS);
            f.extra["tried"] = std::to_string(tried);
            return f;
        }
        if (!rec.ub.empty()) {
            std::string argstr;
            for (auto& [k, v] : args) {
                if (!argstr.empty()) argstr += ", ";
                argstr += k + "=" + std::to_string(v);
            }
            auto f = make_find("concolic", laws::CRASH, fn, rec.ub, rec.ub + " on " + argstr,
                               laws::STRENGTH_FINDS);
            f.counterexample = argstr;
            fill_extra(f, &args, z3_keys.contains(key));
            return f;
        }
        if (generated >= max_new) continue;
        for (auto& cond : branch_conditions(fn)) {
            if (generated >= max_new) break;
            auto [nxt, via_z3, was_unsat] = neighbor_for_cond(fn, args, cond);
            if (was_unsat) {
                ++skipped_unsat;
                continue;
            }
            if (!nxt) continue;
            auto nkey = args_key(*nxt);
            if (seen.contains(nkey)) continue;
            ++generated;
            if (via_z3) {
                ++z3_seeds;
                z3_keys.insert(nkey);
            }
            queue.push_back(*nxt);
        }
    }
    auto f = make_find("concolic", laws::CLEAN, fn, "",
                       "no UB in " + std::to_string(tried) + " concolic inputs (not a proof)",
                       laws::STRENGTH_FINDS);
    fill_extra(f);
    return f;
}

}  // namespace

std::vector<Finding> run_concolic(const std::vector<FunctionInfo>& functions, int budget) {
    std::vector<Finding> out;
    for (auto& fn : functions) out.push_back(concolic_function(fn, budget));
    return out;
}

}  // namespace prism
