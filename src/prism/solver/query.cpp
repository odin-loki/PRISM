// Query analysis on Z3 terms: features for the scheduler, the certifiability
// check, normalisation + hashing for the cache, the bit-blaster to CNF, model
// literals and model validation. Everything here runs on the caller's context
// (or a private translated one); nothing is shared across threads.

#ifdef PRISM_HAS_Z3

#include "prism/solver.hpp"
#include "query.hpp"

#include <algorithm>
#include <cctype>
#include <unordered_map>
#include <unordered_set>

namespace prism::solver {

namespace detail {

// Every uninterpreted constant (0-ary app) in DFS pre-order of first occurrence.
std::vector<z3::expr> collect_consts(const z3::expr& f) {
    std::vector<z3::expr> out;
    std::unordered_set<unsigned> seen;
    std::vector<z3::expr> stack{f};
    while (!stack.empty()) {
        z3::expr e = stack.back();
        stack.pop_back();
        if (!seen.insert(e.id()).second) continue;
        if (e.is_app()) {
            if (e.num_args() == 0 && e.decl().decl_kind() == Z3_OP_UNINTERPRETED) {
                out.push_back(e);
                continue;
            }
            for (unsigned i = e.num_args(); i-- > 0;) stack.push_back(e.arg(i));
        } else if (e.is_quantifier()) {
            stack.push_back(e.body());
        }
    }
    return out;
}

std::string const_name(const z3::expr& e) { return e.decl().name().str(); }

}  // namespace detail

using detail::collect_consts;
using detail::const_name;

std::string Features::logic() const {
    if (quant || arith || other_sort) return "ALL";
    std::string l = "QF_";
    if (arrays) l += "A";
    if (uf) l += "UF";
    if (max_bv_width > 0 || (!fp)) l += "BV";
    if (fp) l += "FP";
    return l;
}

std::string Features::bucket() const {
    auto w = max_bv_width <= 8 ? "w8" : max_bv_width <= 32 ? "w32" : max_bv_width <= 64 ? "w64" : "wide";
    auto n = nodes <= 100 ? "n100" : nodes <= 1000 ? "n1k" : nodes <= 10000 ? "n10k" : "nbig";
    std::string b = logic() + "|" + w + "|" + n;
    if (bv_mul_div) b += "|muldiv";
    return b;
}

Features features(const z3::expr& f) {
    Features ft;
    std::unordered_set<unsigned> seen;
    std::vector<z3::expr> stack{f};
    while (!stack.empty()) {
        z3::expr e = stack.back();
        stack.pop_back();
        if (!seen.insert(e.id()).second) continue;
        ++ft.nodes;
        if (e.is_quantifier()) { ft.quant = true; stack.push_back(e.body()); continue; }
        if (!e.is_app()) { ft.other_sort = true; continue; }  // bound variable
        z3::sort s = e.get_sort();
        switch (s.sort_kind()) {
            case Z3_BOOL_SORT: break;
            case Z3_BV_SORT: ft.max_bv_width = std::max(ft.max_bv_width, s.bv_size()); break;
            case Z3_ARRAY_SORT: ft.arrays = true; break;
            case Z3_FLOATING_POINT_SORT:
            case Z3_ROUNDING_MODE_SORT: ft.fp = true; break;
            case Z3_INT_SORT:
            case Z3_REAL_SORT: ft.arith = true; break;
            default: ft.other_sort = true; break;
        }
        auto k = e.decl().decl_kind();
        if (k == Z3_OP_UNINTERPRETED) {
            if (e.num_args() == 0) ++ft.consts;
            else ft.uf = true;
        }
        switch (k) {
            case Z3_OP_BMUL: case Z3_OP_BUDIV: case Z3_OP_BSDIV: case Z3_OP_BUREM:
            case Z3_OP_BSREM: case Z3_OP_BSMOD: case Z3_OP_BUDIV_I: case Z3_OP_BSDIV_I:
            case Z3_OP_BUREM_I: case Z3_OP_BSREM_I: case Z3_OP_BSMOD_I:
                ft.bv_mul_div = true;
                break;
            default: break;
        }
        for (unsigned i = 0; i < e.num_args(); ++i) {
            z3::sort as = e.arg(i).get_sort();
            if (as.sort_kind() == Z3_ARRAY_SORT) ft.arrays = true;
            stack.push_back(e.arg(i));
        }
    }
    return ft;
}

std::string not_certifiable_reason(const z3::expr& f) {
    if (!f.is_bool()) return "formula is not Boolean";
    auto ft = features(f);
    if (ft.quant) return "quantifiers";
    if (ft.arrays) return "arrays (memory must be Ackermannised to bitvectors first)";
    if (ft.fp) return "floating point";
    if (ft.arith) return "integer/real arithmetic";
    if (ft.uf) return "uninterpreted functions";
    if (ft.other_sort) return "unsupported sort";
    return {};
}

std::string normalized_query(z3::context& src, const z3::expr& f0, std::vector<std::string>* canonical) {
    // A fresh private context (the simplifier and printer order terms by AST
    // id, which depends on everything a context has seen) and the default
    // rewriter with fixed parameters: the same formula always normalises to
    // the same text on one Z3 version.
    z3::context c;
    z3::expr f(c, Z3_translate(src, f0, c));
    z3::expr s = f.simplify();
    auto order = collect_consts(s);
    std::unordered_set<unsigned> have;
    for (const auto& e : order) have.insert(e.id());
    for (const auto& e : collect_consts(f))
        if (have.insert(e.id()).second) order.push_back(e);
    z3::expr_vector from(c), to(c);
    if (canonical) canonical->clear();
    for (std::size_t i = 0; i < order.size(); ++i) {
        from.push_back(order[i]);
        to.push_back(c.constant(("prism!v" + std::to_string(i)).c_str(), order[i].get_sort()));
        if (canonical) canonical->push_back(const_name(order[i]));
    }
    z3::expr r = order.empty() ? s : s.substitute(from, to);
    Z3_string txt = Z3_benchmark_to_smtlib_string(c, "", "", "", "", 0, nullptr, r);
    return std::string("prism-solver-query v1\nz3 ") + Z3_get_full_version() + "\n" + txt;
}

std::string query_hash(z3::context& c, const z3::expr& f) {
    return sha256_hex(normalized_query(c, f));
}

// ---------------------------------------------------------------- bit-blast

std::optional<Cnf> bitblast(z3::context& src, const z3::expr& f0, std::string* why) {
    if (auto r = not_certifiable_reason(f0); !r.empty()) {
        if (why) *why = "not bit-blastable: " + r;
        return std::nullopt;
    }
    // Always bit-blast in a fresh private context: the tactics order terms by
    // AST id, and ids depend on everything a context has seen. A fresh context
    // makes the CNF a function of the formula alone, so a cached certificate
    // can be matched against a re-derived CNF byte for byte.
    z3::context c;
    z3::expr f(c, Z3_translate(src, f0, c));
    return detail::bitblast_fresh(c, f, why);
}

namespace detail {
std::optional<Cnf> bitblast_fresh(z3::context& c, const z3::expr& f, std::string* why) {
    auto fail = [&](std::string m) -> std::optional<Cnf> {
        if (why) *why = std::move(m);
        return std::nullopt;
    };
    Cnf cnf;
    std::unordered_map<unsigned, int> var_of;  // Bool constant ast id -> DIMACS var
    z3::expr_vector from(c), to(c);
    for (const auto& k : collect_consts(f)) {
        Cnf::Symbol sym;
        sym.name = const_name(k);
        if (k.is_bool()) {
            sym.is_bool = true;
            sym.width = 1;
            int v = ++cnf.num_vars;
            var_of[k.id()] = v;
            sym.vars.push_back(v);
        } else {
            sym.width = k.get_sort().bv_size();
            // Split x into Boolean bits x!b0..x!b(w-1) so the CNF's variables
            // map back to x by construction, not through Z3's model converter.
            z3::expr cat(c);
            for (unsigned i = 0; i < sym.width; ++i) {
                std::string bn = "prism!bit!" + std::to_string(cnf.symbols.size()) + "!" + std::to_string(i);
                z3::expr b = c.bool_const(bn.c_str());
                int v = ++cnf.num_vars;
                var_of[b.id()] = v;
                sym.vars.push_back(v);
                z3::expr bit = z3::ite(b, c.bv_val(1, 1), c.bv_val(0, 1));
                cat = i == 0 ? bit : z3::concat(bit, cat);
            }
            from.push_back(k);
            to.push_back(cat);
        }
        cnf.symbols.push_back(std::move(sym));
    }
    z3::expr g0 = f;
    if (!from.empty()) g0 = g0.substitute(from, to);
    z3::goal g(c);
    g.add(g0);
    z3::tactic t = z3::tactic(c, "simplify") & z3::tactic(c, "bit-blast") &
                   z3::tactic(c, "simplify") & z3::tactic(c, "tseitin-cnf");
    z3::apply_result ar = t(g);
    if (ar.size() != 1) return fail("bit-blast produced " + std::to_string(ar.size()) + " goals");
    z3::goal out = ar[0];
    if (out.is_decided_unsat()) {
        cnf.clauses.push_back({});  // the empty clause, exactly as decided
        return cnf;
    }
    auto lit_of = [&](const z3::expr& a, bool& is_const, bool& const_val) -> int {
        bool neg = false;
        z3::expr x = a;
        if (x.is_app() && x.decl().decl_kind() == Z3_OP_NOT) { neg = true; x = x.arg(0); }
        is_const = false;
        if (x.is_true() || x.is_false()) {
            is_const = true;
            const_val = x.is_true() != neg;
            return 0;
        }
        if (!(x.is_app() && x.num_args() == 0 && x.decl().decl_kind() == Z3_OP_UNINTERPRETED && x.is_bool()))
            return 0;
        auto it = var_of.find(x.id());
        int v;
        if (it == var_of.end()) { v = ++cnf.num_vars; var_of[x.id()] = v; }
        else v = it->second;
        return neg ? -v : v;
    };
    for (unsigned i = 0; i < out.size(); ++i) {
        z3::expr cl = out[i];
        std::vector<z3::expr> lits;
        if (cl.is_app() && cl.decl().decl_kind() == Z3_OP_OR)
            for (unsigned j = 0; j < cl.num_args(); ++j) lits.push_back(cl.arg(j));
        else
            lits.push_back(cl);
        std::vector<int> clause;
        bool satisfied = false;
        for (const auto& l : lits) {
            bool is_const = false, val = false;
            int d = lit_of(l, is_const, val);
            if (is_const) { if (val) satisfied = true; continue; }
            if (d == 0) return fail("tactic output is not in clause form: " + l.to_string().substr(0, 120));
            clause.push_back(d);
        }
        if (!satisfied) cnf.clauses.push_back(std::move(clause));
    }
    return cnf;
}
}  // namespace detail

// ---------------------------------------------------------------- model literals
namespace detail {

std::optional<z3::expr> parse_literal(z3::context& c, const z3::sort& s, std::string_view v) {
    auto trim = [](std::string_view x) {
        while (!x.empty() && std::isspace(static_cast<unsigned char>(x.front()))) x.remove_prefix(1);
        while (!x.empty() && std::isspace(static_cast<unsigned char>(x.back()))) x.remove_suffix(1);
        return x;
    };
    v = trim(v);
    if (s.is_bool()) {
        if (v == "true") return c.bool_val(true);
        if (v == "false") return c.bool_val(false);
        return std::nullopt;
    }
    if (!s.is_bv()) return std::nullopt;
    const unsigned w = s.bv_size();
    std::vector<bool> bits;  // LSB first
    if (v.size() > 2 && v[0] == '#' && v[1] == 'b') {
        for (std::size_t i = v.size(); i-- > 2;) {
            if (v[i] != '0' && v[i] != '1') return std::nullopt;
            bits.push_back(v[i] == '1');
        }
    } else if (v.size() > 2 && v[0] == '#' && v[1] == 'x') {
        for (std::size_t i = v.size(); i-- > 2;) {
            int d = std::isdigit(static_cast<unsigned char>(v[i])) ? v[i] - '0'
                    : (v[i] >= 'a' && v[i] <= 'f') ? v[i] - 'a' + 10
                    : (v[i] >= 'A' && v[i] <= 'F') ? v[i] - 'A' + 10 : -1;
            if (d < 0) return std::nullopt;
            for (int b = 0; b < 4; ++b) bits.push_back((d >> b) & 1);
        }
    } else if (v.starts_with("(_") && v.back() == ')') {
        // (_ bvN W)
        auto in = trim(v.substr(2, v.size() - 3));
        if (!in.starts_with("bv")) return std::nullopt;
        in.remove_prefix(2);
        auto sp = in.find(' ');
        if (sp == std::string_view::npos) return std::nullopt;
        std::string num(in.substr(0, sp));
        std::string width(trim(in.substr(sp + 1)));
        if (num.empty() || !std::all_of(num.begin(), num.end(), ::isdigit)) return std::nullopt;
        if (width != std::to_string(w)) return std::nullopt;
        return c.bv_val(num.c_str(), w);
    } else {
        return std::nullopt;
    }
    if (bits.size() != w) return std::nullopt;
    std::unique_ptr<bool[]> raw(new bool[w]);
    for (unsigned i = 0; i < w; ++i) raw[i] = bits[i];
    return z3::expr(c, Z3_mk_bv_numeral(c, w, raw.get()));
}

}  // namespace detail

bool validate_model(z3::context& c, const z3::expr& f, const std::map<std::string, std::string>& model,
                    std::string* why) {
    auto fail = [&](std::string m) {
        if (why) *why = std::move(m);
        return false;
    };
    try {
        z3::model m(c);
        for (const auto& k : collect_consts(f)) {
            auto it = model.find(const_name(k));
            if (it == model.end()) continue;  // completion: any value
            auto val = detail::parse_literal(c, k.get_sort(), it->second);
            if (!val) return fail("malformed value for " + it->first + ": " + it->second.substr(0, 80));
            z3::func_decl d = k.decl();
            m.add_const_interp(d, *val);
        }
        z3::expr r = m.eval(f, true);
        if (r.is_true()) return true;
        return fail("model does not satisfy the formula (evaluates to " + r.to_string().substr(0, 80) + ")");
    } catch (const z3::exception& e) {
        return fail(std::string("z3: ") + e.msg());
    }
}

}  // namespace prism::solver

#endif  // PRISM_HAS_Z3
