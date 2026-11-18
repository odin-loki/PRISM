// prism::sim Lean goal adapter: a small parser for the decidable fragment.

#include "prism/sim/lean_goal.hpp"

#include <cctype>
#include <map>
#include <stdexcept>

namespace prism::sim {
namespace {

struct fail_t : std::runtime_error { using std::runtime_error::runtime_error; };
[[noreturn]] void bail(const std::string& m) { throw fail_t(m); }

// ---- tokens ----
struct tok { enum k_t { id, num, sym, end } k = end; std::string s; };

std::vector<tok> lex(std::string_view in) {
    std::vector<tok> out;
    std::size_t i = 0;
    auto starts = [&](std::string_view p) { return in.substr(i, p.size()) == p; };
    static const char* const syms[] = {"<<<", ">>>", "&&&", "|||", "^^^", "~~~", "<->", "->", "<-", "==", "!=", "<=", ">=", "&&", "||", ":=", "/\\", "\\/",
                                       "∀", "∃", "∧", "∨", "¬", "→", "↔", "≠", "≤", "≥", "ℕ", "ℤ", "⇒",
                                       "+", "-", "*", "/", "%", "=", "<", ">", "(", ")", "{", "}", "[", "]", ",", ":", "!", "∘", "·", "⟨", "⟩"};
    while (i < in.size()) {
        const unsigned char c = static_cast<unsigned char>(in[i]);
        if (std::isspace(c)) { ++i; continue; }
        if (starts("--")) { while (i < in.size() && in[i] != '\n') ++i; continue; }
        if (std::isdigit(c)) {
            std::size_t j = i;
            if (starts("0x") || starts("0X")) { j += 2; while (j < in.size() && std::isxdigit(static_cast<unsigned char>(in[j]))) ++j; }
            else while (j < in.size() && std::isdigit(static_cast<unsigned char>(in[j]))) ++j;
            if (j < in.size() && in[j] == '#') {  // 5#8
                ++j;
                while (j < in.size() && std::isdigit(static_cast<unsigned char>(in[j]))) ++j;
            }
            out.push_back({tok::num, std::string(in.substr(i, j - i))});
            i = j;
            continue;
        }
        if (std::isalpha(c) || c == '_') {
            std::size_t j = i;
            while (j < in.size() && (std::isalnum(static_cast<unsigned char>(in[j])) || in[j] == '_' || in[j] == '\'' || in[j] == '.' ||
                                     static_cast<unsigned char>(in[j]) == 0xCE || static_cast<unsigned char>(in[j]) == 0xCF ||
                                     (static_cast<unsigned char>(in[j]) >= 0x80 && static_cast<unsigned char>(in[j]) <= 0xBF))) {
                if (in[j] == '.' && (j + 1 >= in.size() || !(std::isalpha(static_cast<unsigned char>(in[j + 1])) || in[j + 1] == '_'))) break;
                ++j;
            }
            out.push_back({tok::id, std::string(in.substr(i, j - i))});
            i = j;
            continue;
        }
        bool hit = false;
        for (const char* s : syms) {
            const std::string_view sv(s);
            if (starts(sv)) {
                out.push_back({tok::sym, std::string(sv)});
                i += sv.size();
                hit = true;
                break;
            }
        }
        if (!hit) bail("unsupported character in goal");
    }
    out.push_back({tok::end, ""});
    return out;
}

// ---- untyped AST ----
struct ast {
    std::string k;  // "num", "id", "bin", "un", "ite", "app", "forall"
    std::string s;
    std::vector<ast> kids;
};

struct parser {
    std::vector<tok> t;
    std::size_t p = 0;
    const tok& cur() const { return t[p]; }
    bool is(std::string_view s) const { return t[p].k != tok::end && t[p].s == s; }
    bool eat(std::string_view s) { if (is(s)) { ++p; return true; } return false; }
    void expect(std::string_view s) { if (!eat(s)) bail("expected '" + std::string(s) + "'"); }

    static int prec(const std::string& s) {
        if (s == "↔" || s == "<->") return 10;
        if (s == "→" || s == "->" || s == "⇒") return 20;
        if (s == "∨" || s == "\\/" || s == "||") return 30;
        if (s == "∧" || s == "/\\" || s == "&&") return 35;
        if (s == "=" || s == "==" || s == "!=" || s == "≠" || s == "<" || s == ">" || s == "<=" || s == ">=" || s == "≤" || s == "≥") return 50;
        if (s == "|||") return 55;
        if (s == "^^^") return 58;
        if (s == "&&&") return 60;
        if (s == "<<<" || s == ">>>") return 65;
        if (s == "+" || s == "-") return 65;
        if (s == "*" || s == "/" || s == "%") return 70;
        return -1;
    }
    static bool right_assoc(const std::string& s) { return s == "→" || s == "->" || s == "⇒"; }

    ast expr(int minp = 0) {
        ast lhs = unary();
        for (;;) {
            if (cur().k != tok::sym) break;
            const std::string op = cur().s;
            const int pr = prec(op);
            if (pr < 0 || pr < minp) break;
            ++p;
            ast rhs = expr(right_assoc(op) ? pr : pr + 1);
            lhs = ast{"bin", op, {std::move(lhs), std::move(rhs)}};
        }
        return lhs;
    }
    ast unary() {
        if (cur().k == tok::sym) {
            const std::string s = cur().s;
            if (s == "¬" || s == "!" ) { ++p; return ast{"un", "not", {unary()}}; }
            if (s == "~~~") { ++p; return ast{"un", "bvnot", {unary()}}; }
            if (s == "-") { ++p; return ast{"un", "neg", {unary()}}; }
            if (s == "∀") {
                ++p;
                std::vector<ast> vars;
                parse_binders_until_comma(vars);
                expect(",");
                ast body = expr(0);
                vars.push_back(std::move(body));
                return ast{"forall", "", std::move(vars)};
            }
            if (s == "∃") bail("existential goals are not translated");
            if (s == "(") {
                ++p;
                ast e = expr(0);
                expect(")");
                return postfix(std::move(e));
            }
            bail("unexpected '" + s + "'");
        }
        if (cur().k == tok::num) { ast a{"num", cur().s, {}}; ++p; return a; }
        if (cur().k == tok::id) {
            if (cur().s == "if") {
                ++p;
                ast c = expr(0);
                if (!(cur().k == tok::id && cur().s == "then")) bail("expected then");
                ++p;
                ast a = expr(0);
                if (!(cur().k == tok::id && cur().s == "else")) bail("expected else");
                ++p;
                ast b = expr(0);
                return ast{"ite", "", {std::move(c), std::move(a), std::move(b)}};
            }
            ast head{"id", cur().s, {}};
            ++p;
            // application: id arg arg ... where args are atoms
            ast app{"app", head.s, {}};
            while ((cur().k == tok::num) || (cur().k == tok::id && cur().s != "then" && cur().s != "else") || is("(")) {
                if (cur().k == tok::sym) { ++p; ast e = expr(0); expect(")"); app.kids.push_back(std::move(e)); }
                else { app.kids.push_back(ast{cur().k == tok::num ? "num" : "id", cur().s, {}}); ++p; }
            }
            return app.kids.empty() ? head : app;
        }
        bail("unexpected end of goal");
    }
    ast postfix(ast e) { return e; }

    // `x y : T`, `(x y : T)`, ... until the comma of a binder; records "bind" nodes (names..., type)
    void parse_binders_until_comma(std::vector<ast>& out) {
        for (;;) {
            if (is(",")) return;
            const bool paren = eat("(");
            std::vector<std::string> names;
            while (cur().k == tok::id) { names.push_back(cur().s); ++p; }
            if (names.empty()) bail("bad binder");
            expect(":");
            const std::string ty = type_text();
            if (paren) expect(")");
            for (auto& n : names) out.push_back(ast{"bind", n, {ast{"id", ty, {}}}});
        }
    }
    // consumes a type expression up to `)`, `,`, `:=` or end (not inside parens)
    std::string type_text() {
        std::string s;
        int depth = 0;
        while (cur().k != tok::end) {
            if (depth == 0 && (is(")") || is(",") || is(":=") || is("}") || is("]"))) break;
            if (is("(")) ++depth;
            if (is(")")) --depth;
            if (!s.empty()) s += ' ';
            s += cur().s;
            ++p;
        }
        return s;
    }
};

// ---- elaboration ----
struct scope {
    std::map<std::string, std::pair<std::uint32_t, sort_t>> vars;  // name -> node id, sort
};

struct elab_t {
    term t;
    scope sc;
    std::uint32_t nvars = 0;

    std::uint32_t add(node n) { t.nodes.push_back(std::move(n)); return static_cast<std::uint32_t>(t.nodes.size() - 1); }
    std::uint32_t mk(op o, sort_t s, std::vector<std::uint32_t> kids, std::uint32_t aux = 0) {
        node n;
        n.o = o; n.s = s; n.kids = std::move(kids); n.aux = aux;
        return add(std::move(n));
    }
    static sort_t boolean() { sort_t s; s.kind = sk::boolean; return s; }

    std::optional<sort_t> parse_type(std::string ty) {
        // normalise spaces
        auto trim = [](std::string s) { while (!s.empty() && s.back() == ' ') s.pop_back(); while (!s.empty() && s.front() == ' ') s.erase(s.begin()); return s; };
        ty = trim(ty);
        sort_t s;
        if (ty == "Bool") { s.kind = sk::boolean; return s; }
        if (ty == "Nat" || ty == "ℕ" || ty == "Int" || ty == "ℤ") { s.kind = sk::integer; return s; }
        if (ty.rfind("BitVec ", 0) == 0) {
            const std::string n = trim(ty.substr(7));
            if (n.empty() || !std::isdigit(static_cast<unsigned char>(n[0]))) bail("symbolic BitVec width");
            s.kind = sk::bv;
            s.width = static_cast<std::uint32_t>(std::stoul(n));
            if (s.width == 0 || s.width > 4096) bail("BitVec width out of range");
            return s;
        }
        if (ty.rfind("Fin ", 0) == 0) { s.kind = sk::integer; return s; }
        return std::nullopt;
    }

    std::uint32_t declare(const std::string& name, const sort_t& s) {
        node n;
        n.o = op::var; n.s = s; n.aux = nvars++;
        const auto id = add(std::move(n));
        sc.vars[name] = {id, s};
        return id;
    }

    word parse_num(const std::string& s, std::string* width) {
        std::string d = s;
        const auto h = d.find('#');
        if (h != std::string::npos) { *width = d.substr(h + 1); d = d.substr(0, h); }
        word v = 0;
        if (d.size() > 2 && (d[1] == 'x' || d[1] == 'X')) { for (std::size_t i = 2; i < d.size(); ++i) v = v * 16 + static_cast<word>(std::isdigit(static_cast<unsigned char>(d[i])) ? d[i] - '0' : (std::tolower(d[i]) - 'a' + 10)); }
        else for (char c : d) v = v * 10 + static_cast<word>(c - '0');
        return v;
    }

    std::uint32_t lit(const std::string& text, const std::optional<sort_t>& want) {
        std::string w;
        const word v = parse_num(text, &w);
        node n;
        if (!w.empty()) {
            n.o = op::lit; n.s.kind = sk::bv; n.s.width = static_cast<std::uint32_t>(std::stoul(w));
            if (n.s.width > kMaxWidth) bail("literal wider than the evaluation word");
            n.val = v & (n.s.width >= kMaxWidth ? ~word(0) : ((word(1) << n.s.width) - 1));
        } else if (want && want->kind == sk::bv) {
            n.o = op::lit; n.s = *want;
            if (n.s.width > kMaxWidth) bail("literal wider than the evaluation word");
            n.val = v & (n.s.width >= kMaxWidth ? ~word(0) : ((word(1) << n.s.width) - 1));
        } else {
            n.o = op::ilit; n.s.kind = sk::integer; n.val = v;
        }
        return add(std::move(n));
    }

    bool bare_num(const ast& a) { return a.k == "num" && a.s.find('#') == std::string::npos; }

    // Elaborate; `want` is the sort the context expects (for bare numerals).
    std::uint32_t go(const ast& a, std::optional<sort_t> want = std::nullopt) {
        if (a.k == "num") return lit(a.s, want);
        if (a.k == "id" || (a.k == "app" && a.kids.empty())) {
            if (a.s == "true" || a.s == "True") return mk(op::tru, boolean(), {});
            if (a.s == "false" || a.s == "False") return mk(op::fls, boolean(), {});
            auto it = sc.vars.find(a.s);
            if (it == sc.vars.end()) bail("unknown identifier " + a.s);
            return it->second.first;
        }
        if (a.k == "app") bail("function application " + a.s + " is not translated");
        if (a.k == "ite") {
            const auto c = go(a.kids[0]);
            const auto x = go(a.kids[1], want);
            const auto y = go(a.kids[2], t.nodes[x].s);
            return mk(op::ite, t.nodes[x].s, {c, x, y});
        }
        if (a.k == "un") {
            const auto x = go(a.kids[0], want);
            const sort_t s = t.nodes[x].s;
            if (a.s == "not") return mk(op::not_, boolean(), {x});
            if (a.s == "bvnot") { if (s.kind != sk::bv) bail("~~~ on a non-BitVec"); return mk(op::bvnot, s, {x}); }
            if (s.kind == sk::bv) return mk(op::bvneg, s, {x});
            if (s.kind == sk::integer) return mk(op::ineg, s, {x});
            bail("negation of a non-numeric term");
        }
        if (a.k == "forall") {
            // binders then body
            std::vector<std::uint32_t> hyps;
            for (std::size_t i = 0; i + 1 < a.kids.size(); ++i) {
                auto s = parse_type(a.kids[i].kids[0].s);
                if (!s) bail("unsupported type " + a.kids[i].kids[0].s);
                declare(a.kids[i].s, *s);
            }
            return go(a.kids.back());
        }
        // binary
        const std::string& o = a.s;
        const bool logic = Prec(o) == 1;
        if (logic) {
            const auto x = go(a.kids[0]);
            const auto y = go(a.kids[1]);
            if (t.nodes[x].s.kind != sk::boolean || t.nodes[y].s.kind != sk::boolean) bail("logical connective on a non-proposition");
            if (o == "∧" || o == "/\\" || o == "&&") return mk(op::and_, boolean(), {x, y});
            if (o == "∨" || o == "\\/" || o == "||") return mk(op::or_, boolean(), {x, y});
            if (o == "↔" || o == "<->") return mk(op::eq, boolean(), {x, y});
            return mk(op::implies, boolean(), {x, y});
        }
        // typed binary operators: infer the sort from whichever side is not a bare numeral
        std::uint32_t x, y;
        const bool shift = o == "<<<" || o == ">>>";
        if (bare_num(a.kids[0]) && !bare_num(a.kids[1])) { y = go(a.kids[1], shift ? want : want); x = go(a.kids[0], shift ? t.nodes[y].s : t.nodes[y].s); }
        else { x = go(a.kids[0], want); y = go(a.kids[1], t.nodes[x].s); }
        const sort_t s = t.nodes[x].s;
        const sort_t sy = t.nodes[y].s;
        if (s.kind != sy.kind || (s.kind == sk::bv && s.width != sy.width)) bail("operands of " + o + " differ in sort");
        const bool bv = s.kind == sk::bv, in = s.kind == sk::integer;
        if (o == "=" || o == "==") return mk(op::eq, boolean(), {x, y});
        if (o == "!=" || o == "≠") { const auto e = mk(op::eq, boolean(), {x, y}); return mk(op::not_, boolean(), {e}); }
        if (o == "<" || o == ">" || o == "<=" || o == ">=" || o == "≤" || o == "≥") {
            const bool lt = o == "<", gt = o == ">", le = o == "<=" || o == "≤";
            if (bv) return mk(lt ? op::bvult : gt ? op::bvugt : le ? op::bvule : op::bvuge, boolean(), {x, y});
            if (in) return mk(lt ? op::ilt : gt ? op::igt : le ? op::ile : op::ige, boolean(), {x, y});
            bail("comparison on a non-numeric sort");
        }
        if (bv) {
            if (o == "+") return mk(op::bvadd, s, {x, y});
            if (o == "-") return mk(op::bvsub, s, {x, y});
            if (o == "*") return mk(op::bvmul, s, {x, y});
            if (o == "/") return mk(op::bvudiv, s, {x, y});
            if (o == "%") return mk(op::bvurem, s, {x, y});
            if (o == "&&&") return mk(op::bvand, s, {x, y});
            if (o == "|||") return mk(op::bvor, s, {x, y});
            if (o == "^^^") return mk(op::bvxor, s, {x, y});
            if (o == "<<<") return mk(op::bvshl, s, {x, y});
            if (o == ">>>") return mk(op::bvlshr, s, {x, y});
        }
        if (in) {
            if (o == "+") return mk(op::iadd, s, {x, y});
            if (o == "*") return mk(op::imul, s, {x, y});
            if (o == "-") return mk(op::isub, s, {x, y});  // Nat subtraction is truncated in Lean; see the header note
            if (o == "/") return mk(op::idiv, s, {x, y});
            if (o == "%") return mk(op::imod, s, {x, y});
        }
        if (s.kind == sk::boolean) {
            if (o == "&&&" || o == "&&") return mk(op::and_, s, {x, y});
        }
        bail("operator " + o + " on this sort is not translated");
    }
    static int Prec(const std::string& o) {
        return (o == "∧" || o == "/\\" || o == "&&" || o == "∨" || o == "\\/" || o == "||" || o == "↔" || o == "<->" || o == "→" ||
                o == "->" || o == "⇒")
                   ? 1
                   : 0;
    }
};

}  // namespace

std::optional<term> lean_goal_to_term(std::string_view statement, std::string* why) {
    try {
        auto toks = lex(statement);
        parser ps{std::move(toks), 0};
        elab_t el;
        // header: theorem|lemma|example [name] binders : goal
        if (ps.cur().k == tok::id && (ps.cur().s == "theorem" || ps.cur().s == "lemma" || ps.cur().s == "example")) {
            const bool ex = ps.cur().s == "example";
            ++ps.p;
            if (!ex && ps.cur().k == tok::id) ++ps.p;  // name
            std::vector<std::uint32_t> hyps;
            while (ps.is("(") || ps.is("{") || ps.is("[")) {
                const bool inst = ps.is("[");
                const std::string close = ps.is("(") ? ")" : ps.is("{") ? "}" : "]";
                ++ps.p;
                if (inst) { while (!ps.is("]") && ps.cur().k != tok::end) ++ps.p; ps.expect("]"); continue; }
                std::vector<std::string> names;
                while (ps.cur().k == tok::id) { names.push_back(ps.cur().s); ++ps.p; }
                ps.expect(":");
                // a hypothesis if the name starts with h and the text after ':' parses as a proposition; else a type
                const std::size_t save = ps.p;
                const std::string ty = ps.type_text();
                if (auto s = el.parse_type(ty)) {
                    ps.expect(close);
                    for (auto& n : names) el.declare(n, *s);
                } else {
                    ps.p = save;
                    ast h = ps.expr(0);
                    ps.expect(close);
                    hyps.push_back(el.go(h));
                }
            }
            ps.expect(":");
            ast goal = ps.expr(0);
            std::uint32_t g = el.go(goal);
            if (el.t.nodes[g].s.kind != sk::boolean) bail("the goal is not a proposition");
            if (!hyps.empty()) {
                std::uint32_t h = hyps.size() == 1 ? hyps[0] : el.mk(op::and_, elab_t::boolean(), hyps);
                g = el.mk(op::implies, elab_t::boolean(), {h, g});
            }
            el.t.root = g;
        } else {
            ast goal = ps.expr(0);
            const std::uint32_t g = el.go(goal);
            if (el.t.nodes[g].s.kind != sk::boolean) bail("the goal is not a proposition");
            el.t.root = g;
        }
        el.t.n_vars = el.nvars;
        // nodes were appended in dependency order, but the root must be last: reorder by extraction
        // through canonicalise, which rebuilds the reachable DAG in postorder from the root.
        term out = canonicalise(el.t);
        return out;
    } catch (const fail_t& e) {
        if (why) *why = e.what();
        return std::nullopt;
    } catch (const std::exception& e) {
        if (why) *why = std::string("parse error: ") + e.what();
        return std::nullopt;
    }
}

}  // namespace prism::sim
