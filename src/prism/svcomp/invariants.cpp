// Correctness-witness invariants (`prism svcomp`).
//
// Invariant conjuncts the witness may carry. The engine proves its
// invariants in C semantics over bit-vectors, where a signed `+`, `-` or `*`
// in the invariant text wraps; in C that is an overflow (UB), so a conjunct
// with arithmetic is exported only when the other exported conjuncts bound
// every variable in it so tightly that no subterm leaves the range of int
// (then wrapping and C agree, whatever the variables' integer types). A
// plain comparison of identifiers and constants is always exported. Leaving
// out a conjunct keeps the witness valid (each proved conjunct holds by
// itself); it only gives the validator less to work with.
#include "internal.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <stdexcept>

namespace prism::svcomp {

using namespace detail;
namespace fs = std::filesystem;

namespace {

constexpr long long INT_MIN_ = -(1LL << 31);
constexpr long long INT_MAX_ = (1LL << 31) - 1;

const Regex& atom_re() {
    static const Regex re(R"(^\(?\s*([A-Za-z_]\w*|-?\d+)\s*(<=|>=|==|!=|<|>)\s*([A-Za-z_]\w*|-?\d+)\s*\)?$)");
    return re;
}

bool is_const(const std::string& s) {
    static const Regex re(R"(-?\d+\z)");
    return fullmatch(re, s);
}

// Constant bounds of variables implied by comparison conjuncts.
Bounds bounds_of(const std::vector<std::string>& atoms) {
    std::map<std::string, long long> lo, hi;
    std::vector<std::string> order;  // first-bound order (the result is a map in effect)
    for (const auto& a : atoms) {
        auto m = atom_re().match_prefix(a);
        if (!m) continue;
        std::string x = m->group(1), op = m->group(2), y = m->group(3);
        if (is_const(x) && !is_const(y)) {
            std::swap(x, y);
            static const std::map<std::string, std::string> flip = {{"<", ">"}, {">", "<"}, {"<=", ">="}, {">=", "<="}};
            if (auto it = flip.find(op); it != flip.end()) op = it->second;
        }
        // Only `variable op constant`: a bound carried through `x <= y` could be
        // wrong when x and y differ in signedness (C compares them unsigned).
        if (is_const(x) || !is_const(y)) continue;
        long long c = parse_ll(y).value_or(0);
        auto set_hi = [&](long long v) { hi[x] = hi.contains(x) ? std::min(hi[x], v) : v; };
        auto set_lo = [&](long long v) { lo[x] = lo.contains(x) ? std::max(lo[x], v) : v; };
        if (op == "<=" || op == "==") set_hi(c);
        if (op == "<") set_hi(c - 1);
        if (op == ">=" || op == "==") set_lo(c);
        if (op == ">") set_lo(c + 1);
    }
    std::set<std::string> vars;
    for (auto& [k, _] : lo) vars.insert(k);
    for (auto& [k, _] : hi) vars.insert(k);
    Bounds out;
    for (const auto& v : vars) {
        std::optional<long long> l, h;
        if (lo.contains(v)) l = lo[v];
        if (hi.contains(v)) h = hi[v];
        out.emplace_back(v, std::make_pair(l, h));
    }
    return out;
}

struct Unsafe {};

}  // namespace

bool int_safe(const std::string& expr, const Bounds& bounds) {
    static const Regex tok_re(R"(\d+|[A-Za-z_]\w*|[-+*()]|\S)");
    static const Regex ident(R"([A-Za-z_]\w*\z)");
    std::vector<std::string> toks;
    for (const auto& m : tok_re.finditer(expr)) toks.push_back(m.text);
    std::size_t pos = 0;
    using Iv = std::pair<long long, long long>;
    auto ok = [](Iv iv) -> Iv {
        if (iv.first < INT_MIN_ || iv.second > INT_MAX_) throw Unsafe{};
        return iv;
    };
    std::function<Iv()> add;
    std::function<Iv()> atom = [&]() -> Iv {
        if (pos >= toks.size()) throw Unsafe{};
        const std::string t = toks[pos++];
        if (t == "(") {
            Iv v = add();
            if (pos >= toks.size() || toks[pos] != ")") throw Unsafe{};
            ++pos;
            return v;
        }
        if (t == "-") {
            Iv a = atom();
            return ok({-a.second, -a.first});
        }
        if (!t.empty() && std::all_of(t.begin(), t.end(), [](char c) { return c >= '0' && c <= '9'; })) {
            long long v = parse_ll(t).value_or(1LL << 62);
            return ok({v, v});
        }
        if (fullmatch(ident, t)) {
            auto it = std::find_if(bounds.begin(), bounds.end(), [&](const auto& b) { return b.first == t; });
            if (it == bounds.end() || !it->second.first || !it->second.second) throw Unsafe{};
            return ok({*it->second.first, *it->second.second});
        }
        throw Unsafe{};
    };
    auto mul = [&]() -> Iv {
        Iv v = atom();
        while (pos < toks.size() && toks[pos] == "*") {
            ++pos;
            Iv w = atom();
            long long ps[4] = {v.first * w.first, v.first * w.second, v.second * w.first, v.second * w.second};
            v = ok({*std::min_element(ps, ps + 4), *std::max_element(ps, ps + 4)});
        }
        return v;
    };
    add = [&]() -> Iv {
        Iv v = mul();
        while (pos < toks.size() && (toks[pos] == "+" || toks[pos] == "-")) {
            std::string op = toks[pos++];
            Iv w = mul();
            v = ok(op == "+" ? Iv{v.first + w.first, v.second + w.second} : Iv{v.first - w.second, v.second - w.first});
        }
        return v;
    };
    try {
        add();
    } catch (const Unsafe&) {
        return false;
    }
    return pos == toks.size();
}

std::vector<std::string> exportable_conjuncts(const std::vector<std::string>& conjuncts) {
    std::vector<std::string> atoms;
    for (const auto& c : conjuncts)
        if (atom_re().match_prefix(c)) atoms.push_back(c);
    Bounds b = bounds_of(atoms);
    static const Regex cmp(R"(^(.*?)\s*(<=|>=|==|!=|<|>)\s*(.*)$)");
    static const Regex rel_chars(R"([<>=!])");
    static const Regex simple(R"((?:[A-Za-z_]\w*|-?\d+)\z)");
    std::vector<std::string> out;
    for (const auto& c : conjuncts) {
        if (atom_re().match_prefix(c)) {
            out.push_back(c);
            continue;
        }
        auto m = cmp.match_prefix(c);
        if (!m || rel_chars.search(m->group(1) + m->group(3))) continue;
        bool all = true;
        for (const auto& side : {m->group(1), m->group(3)})
            if (!(fullmatch(simple, strip(side)) || int_safe(side, b))) {
                all = false;
                break;
            }
        if (all) out.push_back(c);
    }
    return out;
}

// Only invariants the engine proved are exported: the Houdini-filtered loop
// invariants of a bmc PROVED-UNBOUNDED (extra["invariants"], one list per
// cut loop, with the loops' source positions in extra["invariant_loops"]),
// or pir's structured conjuncts rendered in C (debug_ir.cpp). A loop whose
// position is unknown (inlined body), that is a `do` loop (its invariant is
// proved at the top of the body, not where the condition is evaluated), or
// whose keyword is not at that position in the task text gets none. Every
// other proof (pir PROVED within the unwind, k-induction without an
// invariant) exports nothing, and the witness is the empty invariant_set:
// trivially valid, the validator has to find the proof itself.
std::pair<std::vector<Invariant>, std::string> correctness_invariants(const fs::path& task, const json& finding) {
    const json& extra = extra_of(finding);
    if (get_str(extra, "k_induction", "None") != "closed-invariants" ||
        !(has(extra, "invariants") || has(extra, "invariant_conjuncts")))
        return {{}, "no loop invariant exported by the proving stage (empty invariant set)"};
    const bool pir = !has(extra, "invariants");
    std::vector<std::vector<std::string>> invs;
    json loops;
    try {
        if (!pir) {
            json raw = json::parse(pystr(extra.at("invariants")));
            if (!raw.is_array()) throw std::invalid_argument("invariants");
            for (const auto& list : raw) {
                std::vector<std::string> row;
                if (list.is_array())
                    for (const auto& e : list) row.push_back(pystr(e));
                invs.push_back(std::move(row));
            }
        } else {
            // pir: structured conjuncts over IR values, rendered in C here
            auto texts = pir_invariant_texts(task, finding);
            if (!texts) return {{}, "pir invariants not mapped to C (no debug information; empty invariant set)"};
            invs = std::move(*texts);
        }
        loops = json::parse(has(extra, "invariant_loops") ? pystr(extra.at("invariant_loops")) : "[]");
    } catch (const std::exception&) {
        return {{}, "unreadable invariants (empty invariant set)"};
    }
    const auto lines = split_nl(read_text(task));
    std::vector<Invariant> out;
    static const Regex kw_re(R"((for|while)\b)");
    static const Regex word_char(R"(\w)");
    static const Regex for_decl(R"(for\s*\(\s*(?:[A-Za-z_]\w*\s+)+\**\s*([A-Za-z_]\w*)\s*=)");
    std::optional<std::string> function;
    if (has(finding, "function") && finding.at("function").is_string() && truthy(finding.at("function")))
        function = finding.at("function").get<std::string>();
    if (loops.is_array()) {
        for (std::size_t j = 0; j < loops.size(); ++j) {
            const json& loop = loops[j];
            if (j >= invs.size() || !loop.is_object()) continue;
            std::string kind = has(loop, "kind") ? pystr(loop.at("kind")) : "None";
            if (kind != "for" && kind != "while" && kind != "") continue;
            long line = has(loop, "line") && truthy(loop.at("line")) ? static_cast<long>(to_int(loop.at("line")).value_or(0)) : 0;
            long col = has(loop, "column") && truthy(loop.at("column")) ? static_cast<long>(to_int(loop.at("column")).value_or(0)) : 0;
            if (!(1 <= line && line <= static_cast<long>(lines.size())) || col < 1) continue;
            const std::string& ltext = lines[static_cast<std::size_t>(line - 1)];
            const std::string at = static_cast<std::size_t>(col - 1) < ltext.size() ? ltext.substr(static_cast<std::size_t>(col - 1)) : "";
            std::string kw = kind;
            if (kw.empty()) {  // pir: the keyword at the loop's start position (a `do` loop gets none)
                auto km = kw_re.match_prefix(at);
                if (!km) continue;
                kw = km->group(1);
            }
            Regex kwb(re_escape(kw) + "\\b");
            if (!kwb.match_prefix(at)) continue;
            if (col > 1 && word_char.match_prefix(std::string_view(ltext).substr(static_cast<std::size_t>(col - 2), 1)))
                continue;
            // a name declared in the for-init is not in scope at the keyword
            auto decl = for_decl.match_prefix(at);
            std::optional<Regex> declared;
            if (decl) declared.emplace("\\b" + re_escape(decl->group(1)) + "\\b");
            std::vector<std::string> mine;
            for (const auto& e : invs[j])
                if (!(declared && declared->search(e))) mine.push_back(strip(e));
            // pir conjuncts are rendered with their C types; bmc's are filtered here
            std::vector<std::string> kept = pir ? mine : exportable_conjuncts(mine);
            if (!kept.empty()) {
                std::string value;
                for (std::size_t k = 0; k < kept.size(); ++k) value += (k ? " && " : "") + ("(" + kept[k] + ")");
                out.push_back(Invariant{"loop_invariant", Location{task.filename().string(), line, col, function}, value});
            }
        }
    }
    std::size_t n = 0;
    for (const auto& inv : out) {
        std::size_t k = 1, at = 0;
        while ((at = inv.value.find(" && ", at)) != std::string::npos) {
            ++k;
            at += 4;
        }
        n += k;
    }
    std::string stage = has(finding, "stage") ? pystr(finding.at("stage")) : "bmc";
    return {out, std::to_string(n) + " Houdini loop invariant conjunct(s) from " + stage};
}

}  // namespace prism::svcomp
