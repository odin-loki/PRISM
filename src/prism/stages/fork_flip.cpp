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
#include "prism/regex.hpp"

#include <cctype>
#include <cstdint>
#include <filesystem>
#include <set>
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

// Words a flip query may hold besides the function's name and parameters:
// C keywords, the hooks and the standard integer type names.
const std::set<std::string, std::less<>>& context_free_words() {
    static const std::set<std::string, std::less<>> w = {
        "int", "unsigned", "signed", "long", "short", "char", "_Bool", "bool", "const", "volatile",
        "void", "static", "inline", "true", "false", "sizeof", "if", "assert", "__VERIFIER_assume",
        "noexcept", "constexpr",
        "int8_t", "int16_t", "int32_t", "int64_t", "uint8_t", "uint16_t", "uint32_t", "uint64_t",
        "size_t", "ptrdiff_t", "intptr_t", "uintptr_t", "intmax_t", "uintmax_t"};
    return w;
}

// The flip query of g without its file, when g's signature and body name
// nothing but g, its parameters and the words above: the function is then
// the same C text over the same types in either unit, so its check does not
// depend on the rest of the file. nullopt otherwise (macros, typedefs,
// globals, callees: the file is needed), or for a function without
// parameters (a parameterless error sink's verdict depends on its callers in
// the unit: pir's caller_discharged_asserts).
std::optional<std::string> context_free_unit(const FunctionInfo& g, const std::string& prelude) {
    if (g.params.empty() || g.name.empty()) return std::nullopt;
    std::set<std::string, std::less<>> ok;
    ok.insert(g.name);
    for (auto& [t, n] : g.params) {
        if (n.empty()) return std::nullopt;
        ok.insert(n);
    }
    const std::string text = g.signature + "\n" + g.body;
    std::set<std::string, std::less<>> used;  // the words the function holds
    for (std::size_t i = 0; i < text.size();) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        if (std::isalpha(c) || c == '_') {
            std::size_t j = i;
            while (j < text.size() && (std::isalnum(static_cast<unsigned char>(text[j])) || text[j] == '_')) ++j;
            auto w = std::string_view(text).substr(i, j - i);
            if (!ok.contains(w) && !context_free_words().contains(w)) return std::nullopt;
            used.emplace(w);
            i = j;
        } else if (std::isdigit(c)) {
            // a number and its suffix (0x1F, 10u, 3ull)
            while (i < text.size() && (std::isalnum(static_cast<unsigned char>(text[i])) || text[i] == '.')) ++i;
        } else if (c == '"' || c == '\'' || c == '#' || c == '\\' || c == '.' || c == '[' || c == '{' ||
                   c == '}' || c == ':') {
            // strings, characters, directives, members, arrays, blocks, scopes:
            // only the plain expression form is taken without its file
            if (c == '{' || c == '}') {
                ++i;
                continue;
            }
            return std::nullopt;
        } else {
            ++i;
        }
    }
    // The file must not change what those words mean: no macro of one of
    // them (`#define int long`), no pragma, no header of the project (which
    // could define such a macro). System headers define none of them but
    // the ones the unit below includes itself.
    const auto src = stages_detail::read_fn_source(g);
    if (src.empty() || src.find("#include \"") != std::string::npos || src.find("pragma") != std::string::npos)
        return std::nullopt;
    static const Regex define_re(R"(^[ \t]*#[ \t]*define[ \t]+([A-Za-z_]\w*))", /*multiline=*/true);
    for (auto& m : define_re.finditer(src))
        if (used.contains(m.group(1))) return std::nullopt;
    // the integer type names (stdint.h, stddef.h) and bool (stdbool.h, C);
    // the definition keeps its #line, so its rows keep their lines
    const auto ext = std::filesystem::path(g.file).extension().string();
    const bool c = ext == ".c" || ext.empty();
    return "#include <assert.h>\n" + prelude + "#include <stdint.h>\n#include <stddef.h>\n" +
           (c ? std::string("#include <stdbool.h>\n") : std::string()) + stages_detail::pir_definition(g);
}

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
        stages_detail::PirQuery q{g, 1, {"assert"}, stages_detail::kPirHookPrelude, std::nullopt};
        // Fast path: the query without its file (context_free_unit), whose
        // answer is the file's own when it is one: a model of our assert, or
        // a proof. Anything else (UNKNOWN, ERROR, NEEDS-HARNESS) is asked
        // again in the file, the answer the stage always had.
        if (auto unit = context_free_unit(g, q.prelude)) {
            auto alone = q;
            alone.unit = std::move(unit);
            auto f = stages_detail::pir_query(alone);
            auto prop = f.extra.find("prop");
            const bool model = f.status == laws::FAILED && prop != f.extra.end() && prop->second == "assert" &&
                               !f.counterexample.empty();
            if (model || f.status == laws::PROVED || f.status == laws::PROVED_UNBOUNDED) return f;
            // the dry run (prefetch) asks the fast query only: a fallback is
            // asked when the real pass needs it
            if (stages_detail::collecting_pir()) return f;
        }
        return stages_detail::pir_query(q);
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
