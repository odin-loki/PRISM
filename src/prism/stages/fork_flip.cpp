// solve_fork_flip on pir (KLEE Executor::fork): the flipped branch condition
// becomes a reachability question about the function's own unit. The body is
// replaced by
//
//   __VERIFIER_assume(<flipped cond>);
//   if (<some parameter differs from the seed>) assert(0);   // pass 1
//   assert(0);                                                // pass 2
//   __VERIFIER_assume(0);
//
// and checked by pir_one: a FAILED `assert` gives the new arguments (the
// counterexample), a proof that it is unreachable in pass 2 means the
// flipped side is infeasible (Unsat: concolic drops it). Anything else
// (a UB of the condition itself, NEEDS-HARNESS, UNKNOWN) is Unknown, and
// concolic falls back to its heuristic flip.
#include "prism/stages.hpp"

#include "common.hpp"
#include "prism/laws.hpp"

#include <cstdint>
#include <sstream>
#include <string>

namespace prism {

namespace {

std::optional<int64_t> cex_int(std::string v) {
    v = stages_detail::strip(v);
    if (v.empty()) return std::nullopt;
    try {
        std::size_t used = 0;
        if (v[0] == '-') {
            auto x = std::stoll(v, &used, 0);
            return used == v.size() ? std::optional<int64_t>(x) : std::nullopt;
        }
        auto x = std::stoull(v, &used, 0);
        return used == v.size() ? std::optional<int64_t>(static_cast<int64_t>(x)) : std::nullopt;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

int to_i32(int64_t x) { return static_cast<int>(static_cast<int32_t>(static_cast<uint32_t>(static_cast<uint64_t>(x)))); }

}  // namespace

ForkFlipResult solve_fork_flip(const FunctionInfo& fn, const std::map<std::string, int>& seed,
                               const std::string& cond, bool want) {
    std::string flip = want ? "!!(" + cond + ")" : "!(" + cond + ")";
    std::string differs;
    for (auto& [typ, name] : fn.params) {
        if (name.empty()) continue;
        auto it = seed.find(name);
        const std::string s = std::to_string(it == seed.end() ? 0 : it->second);
        differs += (differs.empty() ? "" : " || ") + name + " != (" + typ + ")(" + s + ")";
    }
    auto run = [&](bool away_from_seed) {
        FunctionInfo g = fn;
        g.body = "\n    __VERIFIER_assume(" + flip + ");\n";
        if (away_from_seed) g.body += "    if (" + differs + ") assert(0);\n";
        else g.body += "    assert(0);\n";
        g.body += "    __VERIFIER_assume(0);\n";
        return stages_detail::pir_one(g, 1, {"assert"}, stages_detail::kPirHookPrelude);
    };
    auto model_of = [&](const Finding& f) -> std::optional<ForkFlipResult> {
        auto prop = f.extra.find("prop");
        const bool our_assert = prop != f.extra.end() && prop->second == "assert";
        if (f.status != laws::FAILED || !our_assert || f.counterexample.empty()) return std::nullopt;
        ForkFlipResult r;
        r.kind = ForkFlipKind::Model;
        std::istringstream ss(f.counterexample);
        std::string part;
        while (std::getline(ss, part, ',')) {
            auto eq = part.find('=');
            if (eq == std::string::npos) continue;
            auto v = cex_int(part.substr(eq + 1));
            if (!v) return std::nullopt;
            r.args[stages_detail::strip(part.substr(0, eq))] = to_i32(*v);
        }
        for (auto& [typ, name] : fn.params)
            if (!name.empty() && !r.args.count(name)) r.args[name] = seed.count(name) ? seed.at(name) : 0;
        return r;
    };
    if (!differs.empty())
        if (auto m = model_of(run(true))) return *m;
    auto f = run(false);
    if (auto m = model_of(f)) return *m;
    if (f.status == laws::PROVED || f.status == laws::PROVED_UNBOUNDED) return {ForkFlipKind::Unsat, {}};
    return {};
}

}  // namespace prism
