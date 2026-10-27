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

z3::expr portable_smt2(const z3::expr& f) {
    std::unordered_map<unsigned, z3::expr> memo;
    // Iterative post-order: a deep VC must not overflow the stack.
    std::vector<std::pair<z3::expr, bool>> stack{{f, false}};
    while (!stack.empty()) {
        auto [e, expanded] = stack.back();
        stack.pop_back();
        if (memo.count(e.id())) continue;
        if (!e.is_app()) {
            memo.emplace(e.id(), e);
            continue;
        }
        // 0-argument `and`/`or` are apps, so they must be rewritten here.
        // Z3 prints them `(and )` / `(or )`, which Bitwuzla rejects. The
        // identities are true and false.
        if (e.num_args() == 0) {
            const auto k = e.decl().decl_kind();
            if (k == Z3_OP_AND) memo.emplace(e.id(), f.ctx().bool_val(true));
            else if (k == Z3_OP_OR) memo.emplace(e.id(), f.ctx().bool_val(false));
            else memo.emplace(e.id(), e);
            continue;
        }
        if (!expanded) {
            stack.push_back({e, true});
            for (unsigned i = e.num_args(); i-- > 0;)
                if (!memo.count(e.arg(i).id())) stack.push_back({e.arg(i), false});
            continue;
        }
        z3::expr_vector args(f.ctx());
        bool changed = false;
        for (unsigned i = 0; i < e.num_args(); ++i) {
            const z3::expr& a = memo.at(e.arg(i).id());
            changed = changed || !z3::eq(a, e.arg(i));
            args.push_back(a);
        }
        const auto k = e.decl().decl_kind();
        const bool mul_flag =
            (k == Z3_OP_BSMUL_NO_OVFL || k == Z3_OP_BSMUL_NO_UDFL || k == Z3_OP_BUMUL_NO_OVFL) && args.size() == 2;
        if ((k == Z3_OP_OR || k == Z3_OP_AND) && args.size() == 1) {
            memo.emplace(e.id(), args[0]);
        } else if (mul_flag) {
            // Z3's bvsmul_noovfl / bvsmul_noudfl / bvumul_noovfl are not
            // SMT-LIB: the same predicate on the double-width product.
            const unsigned w = args[0].get_sort().bv_size();
            const bool sgn = k != Z3_OP_BUMUL_NO_OVFL;
            const z3::expr a2 = sgn ? z3::sext(args[0], w) : z3::zext(args[0], w);
            const z3::expr b2 = sgn ? z3::sext(args[1], w) : z3::zext(args[1], w);
            const z3::expr p = a2 * b2;
            z3::context& c = f.ctx();
            if (!sgn) {
                memo.emplace(e.id(), p.extract(2 * w - 1, w) == c.bv_val(0, w));
            } else {
                // p fits in w signed bits iff bits [2w-2 : w-1] all equal the sign bit.
                const z3::expr neg = p.extract(2 * w - 1, 2 * w - 1) == c.bv_val(1, 1);
                const z3::expr mid = p.extract(2 * w - 2, w - 1);
                memo.emplace(e.id(), k == Z3_OP_BSMUL_NO_OVFL ? (neg || mid == c.bv_val(0, w))
                                                               : (!neg || mid == ~c.bv_val(0, w)));
            }
        } else {
            memo.emplace(e.id(), changed ? e.decl()(args) : e);
        }
    }
    return memo.at(f.id());
}

namespace {

bool plain_symbol(const std::string& n) {
    return !n.empty() && n.find('|') == std::string::npos && n.find('\\') == std::string::npos;
}

// Named Z3 tactics and the `apply` command. Not SMT-LIB commands.
// `check-sat-using` is rewritten to `(check-sat)` by bitwuzla_dialect.
bool z3_tactic_command(std::string_view head) {
    return head == "apply" || head == "simplify" || head == "bit-blast" || head == "smt" || head == "qfbv" ||
           head == "qfbv-solver" || head == "then" || head == "and-then" || head == "or-else" || head == "par-or" ||
           head == "using-params" || head == "try-for" || head == "tseitin-cnf" || head == "propagate-values" ||
           head == "solve-eqs" || head == "elim-uncnstr" || head == "ctx-solver-simplify" || head == "nnf" ||
           head == "aig";
}

std::string_view head_of(std::string_view cmd) {
    std::size_t i = 0;
    if (i < cmd.size() && cmd[i] == '(') ++i;
    while (i < cmd.size() && std::isspace(static_cast<unsigned char>(cmd[i]))) ++i;
    const std::size_t b = i;
    while (i < cmd.size()) {
        const unsigned char c = static_cast<unsigned char>(cmd[i]);
        if (std::isspace(c) || cmd[i] == '(' || cmd[i] == ')') break;
        ++i;
    }
    return cmd.substr(b, i - b);
}

// End offset of the s-expression starting at `open`, or npos if it is unclosed.
// Strings ("") and quoted symbols (|...|) can hold parentheses; comments run
// to the end of the line.
std::size_t sexpr_end(std::string_view in, std::size_t open) {
    int depth = 0;
    bool in_str = false;
    bool in_bar = false;
    for (std::size_t i = open; i < in.size(); ++i) {
        const char c = in[i];
        if (in_str) {
            if (c == '"') {
                if (i + 1 < in.size() && in[i + 1] == '"') {
                    ++i;
                    continue;
                }
                in_str = false;
            }
            continue;
        }
        if (in_bar) {
            if (c == '|') in_bar = false;
            continue;
        }
        if (c == ';') {
            while (i + 1 < in.size() && in[i + 1] != '\n') ++i;
            continue;
        }
        if (c == '"') {
            in_str = true;
            continue;
        }
        if (c == '|') {
            in_bar = true;
            continue;
        }
        if (c == '(') {
            ++depth;
        } else if (c == ')') {
            --depth;
            if (depth == 0) return i + 1;
        }
    }
    return std::string_view::npos;
}

}  // namespace

std::string bitwuzla_dialect(std::string_view in) {
    std::string out;
    out.reserve(in.size());
    for (std::size_t i = 0; i < in.size();) {
        if (in[i] == ';') {
            const std::size_t e = in.find('\n', i);
            if (e == std::string_view::npos) {
                out.append(in.substr(i));
                break;
            }
            out.append(in.substr(i, e + 1 - i));
            i = e + 1;
            continue;
        }
        if (in[i] != '(') {
            out.push_back(in[i]);
            ++i;
            continue;
        }
        const std::size_t end = sexpr_end(in, i);
        if (end == std::string_view::npos) {
            out.append(in.substr(i));
            break;
        }
        const std::string_view cmd = in.substr(i, end - i);
        const std::string_view head = head_of(cmd);
        if (head == "check-sat-using") out += "(check-sat)";
        else if (!z3_tactic_command(head)) out.append(cmd);
        i = end;
    }
    return out;
}

std::string external_smt2(const z3::expr& f, std::string_view logic) {
    const z3::expr portable = portable_smt2(f);
    const std::string logic_owned(logic);
    std::string txt = "(set-option :produce-models true)\n";
    if (const char* printed = Z3_benchmark_to_smtlib_string(f.ctx(), "prism", logic_owned.c_str(), "unknown", "", 0,
                                                            nullptr, portable))
        txt += printed;
    std::string names;
    for (const auto& k : collect_consts(f)) {
        if (!plain_symbol(const_name(k))) continue;
        if (!names.empty()) names += ' ';
        names += "|" + const_name(k) + "|";
    }
    if (!names.empty()) txt += "(get-value (" + names + "))\n";
    txt += "(exit)\n";
    return bitwuzla_dialect(txt);
}

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
    // The rewriter honours only the context timeout, and nothing else bounded
    // it: on zlib crc32.c one normalisation ran for over 15 minutes, past
    // every --timeout. Past kNormalizeMs the formula keys the cache as it
    // was given (still one text per formula; a slower machine only misses
    // the cache, it never shares an entry between two formulas).
    constexpr unsigned kNormalizeMs = 10000;
    c.set("timeout", static_cast<int>(kNormalizeMs));
    z3::expr s = f;
    try {
        s = f.simplify();
    } catch (const z3::exception&) {
        s = f;
    }
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
    for (unsigned i = 0; i < w; ++i) raw[i] = bits.at(i);
    return z3::expr(c, Z3_mk_bv_numeral(c, w, raw.get()));
}

}  // namespace detail

bool validate_model(z3::context& c, const z3::expr& f, const std::map<std::string, std::string>& model,
                    std::string* why) {
    auto fail = [&](std::string message) {
        if (why) *why = std::move(message);
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
