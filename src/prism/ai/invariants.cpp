// Loop invariant synthesis + Houdini (roadmap 4.2, 8.2 "Houdini", 9.2).
//
// Soundness argument (why a closed result is PROVED-UNBOUNDED): every loop is
// replaced by a loop cut
//     init; assert(I) ; havoc(W) ; assume(I) ;
//     step(c) { body ; incr ; assert(I) }      -- continues under !c
// where W over-approximates everything the loop writes. Every reachable loop
// head state satisfies I (base assert + consecution assert), so every body
// execution and every post-loop state of the real program is represented by
// the cut program. If no property of the cut program (UB checks and the
// asserts of I) is satisfiable, the real function has no encoded UB for any
// number of iterations. Houdini only chooses I: a candidate whose base or
// consecution assert can fail is dropped, until the survivors are inductive
// relative to their conjunction. Loops the cut cannot model soundly (nesting,
// break/continue/goto/switch, calls, pointers, side-effecting conditions) are
// refused and keep their BOUNDED verdict.

#include "ai_internal.hpp"

#include "prism/laws.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <map>
#include <set>

namespace prism::ai {
namespace {

struct Tok {
    std::string t;
    std::size_t pos;
};

std::vector<Tok> lex(const std::string& s) {
    std::vector<Tok> out;
    std::size_t i = 0;
    static const char* ops3[] = {"<<=", ">>=", "..."};
    static const char* ops2[] = {"++", "--", "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", "==", "!=",
                                 "<=", ">=", "&&", "||", "->", "<<", ">>", "::"};
    while (i < s.size()) {
        char c = s[i];
        if (std::isspace(static_cast<unsigned char>(c))) {
            ++i;
            continue;
        }
        if (c == '"' || c == '\'') {
            std::size_t j = i + 1;
            while (j < s.size() && s[j] != c) {
                if (s[j] == '\\') ++j;
                ++j;
            }
            out.push_back({s.substr(i, std::min(j + 1, s.size()) - i), i});
            i = j + 1;
            continue;
        }
        if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
            std::size_t j = i;
            while (j < s.size() && (std::isalnum(static_cast<unsigned char>(s[j])) || s[j] == '_')) ++j;
            out.push_back({s.substr(i, j - i), i});
            i = j;
            continue;
        }
        if (std::isdigit(static_cast<unsigned char>(c))) {
            std::size_t j = i;
            while (j < s.size() && (std::isalnum(static_cast<unsigned char>(s[j])) || s[j] == '.')) ++j;
            out.push_back({s.substr(i, j - i), i});
            i = j;
            continue;
        }
        bool done = false;
        for (auto* o : ops3)
            if (!done && s.compare(i, 3, o) == 0) {
                out.push_back({o, i});
                i += 3;
                done = true;
            }
        for (auto* o : ops2)
            if (!done && s.compare(i, 2, o) == 0) {
                out.push_back({o, i});
                i += 2;
                done = true;
            }
        if (!done) {
            out.push_back({std::string(1, c), i});
            ++i;
        }
    }
    return out;
}

bool is_assign_op(const std::string& t) {
    return t == "=" || t == "+=" || t == "-=" || t == "*=" || t == "/=" || t == "%=" || t == "&=" ||
           t == "|=" || t == "^=" || t == "<<=" || t == ">>=";
}

const std::set<std::string>& type_words() {
    static const std::set<std::string> w{"int", "char", "short", "long", "unsigned", "signed", "void",
                                         "float", "double", "size_t", "ssize_t", "int8_t", "int16_t",
                                         "int32_t", "int64_t", "uint8_t", "uint16_t", "uint32_t",
                                         "uint64_t", "bool", "_Bool", "const", "volatile", "ptrdiff_t",
                                         "intptr_t", "uintptr_t", "struct", "union", "enum"};
    return w;
}

const std::set<std::string>& keywords() {
    static const std::set<std::string> k{"if", "else", "for", "while", "do", "switch", "case", "default",
                                         "return", "break", "continue", "goto", "sizeof", "assert"};
    return k;
}

struct Written {
    std::set<std::string> scalars, arrays;
};

// Lvalues written in a token range: x = / x op= / x++ / ++x / a[..] = / *p =.
Written writes_of(const std::vector<Tok>& tk) {
    Written w;
    for (std::size_t i = 0; i < tk.size(); ++i) {
        auto& t = tk[i].t;
        if (!is_identifier(t) || keywords().count(t) || type_words().count(t)) continue;
        std::size_t j = i + 1;
        bool arr = false;
        if (j < tk.size() && tk[j].t == "[") {
            int depth = 0;
            for (; j < tk.size(); ++j) {
                if (tk[j].t == "[") ++depth;
                else if (tk[j].t == "]" && --depth == 0) break;
            }
            ++j;
            arr = true;
        }
        bool post = j < tk.size() && (is_assign_op(tk[j].t) || tk[j].t == "++" || tk[j].t == "--");
        bool pre = i > 0 && (tk[i - 1].t == "++" || tk[i - 1].t == "--");
        bool deref = i > 0 && tk[i - 1].t == "*" &&
                     (i == 1 || tk[i - 2].t == ";" || tk[i - 2].t == "{" || tk[i - 2].t == "}") &&
                     j < tk.size() && is_assign_op(tk[j].t);
        if (deref) w.arrays.insert(t);
        else if (post || pre) (arr ? w.arrays : w.scalars).insert(t);
    }
    return w;
}

// Declared names in a token range: scalars and fixed-size arrays.
void decls_of(const std::vector<Tok>& tk, std::vector<std::string>& scalars,
              std::vector<std::pair<std::string, int>>& arrays) {
    for (std::size_t i = 0; i + 1 < tk.size(); ++i) {
        if (!type_words().count(tk[i].t)) continue;
        std::size_t j = i + 1;
        while (j < tk.size() && type_words().count(tk[j].t)) ++j;
        // declarator list: name [= ...] (, name ...)*
        while (j < tk.size() && is_identifier(tk[j].t) && !keywords().count(tk[j].t)) {
            auto name = tk[j].t;
            if (j + 1 < tk.size() && tk[j + 1].t == "[") {
                int n = -1;
                if (j + 3 < tk.size() && tk[j + 3].t == "]") {
                    try {
                        n = std::stoi(tk[j + 2].t);
                    } catch (...) {
                    }
                }
                if (n > 0) arrays.push_back({name, n});
            } else if (j + 1 < tk.size() && tk[j + 1].t == "(") {
                break;  // a function declaration, not a variable
            } else {
                scalars.push_back(name);
            }
            // skip to the next top-level comma or statement end
            int depth = 0;
            std::size_t k = j + 1;
            for (; k < tk.size(); ++k) {
                auto& t = tk[k].t;
                if (t == "(" || t == "[" || t == "{") ++depth;
                else if (t == ")" || t == "]" || t == "}") --depth;
                if (depth < 0) break;
                if (depth == 0 && (t == "," || t == ";")) break;
            }
            if (k < tk.size() && tk[k].t == ",") j = k + 1;
            else break;
        }
        i = j;
    }
}

struct Walker {
    const std::string& s;
    std::vector<LoopCut> loops;
    std::string why;

    explicit Walker(const std::string& src) : s(src) {}

    std::size_t skip_ws(std::size_t i) const {
        while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
        return i;
    }
    std::string word_at(std::size_t i) const {
        if (i >= s.size() || !(std::isalpha(static_cast<unsigned char>(s[i])) || s[i] == '_')) return {};
        std::size_t j = i;
        while (j < s.size() && (std::isalnum(static_cast<unsigned char>(s[j])) || s[j] == '_')) ++j;
        return s.substr(i, j - i);
    }
    // index just past the bracket matching the one at i (skips literals).
    std::size_t match(std::size_t i) const {
        char open = s[i], close = open == '(' ? ')' : open == '{' ? '}' : ']';
        int depth = 0;
        for (std::size_t j = i; j < s.size(); ++j) {
            char c = s[j];
            if (c == '"' || c == '\'') {
                std::size_t k = j + 1;
                while (k < s.size() && s[k] != c) {
                    if (s[k] == '\\') ++k;
                    ++k;
                }
                j = k;
                continue;
            }
            if (c == open) ++depth;
            else if (c == close && --depth == 0) return j + 1;
        }
        return std::string::npos;
    }
    // index just past a simple statement ending in ';' at depth 0.
    std::size_t simple_end(std::size_t i) const {
        for (std::size_t j = i; j < s.size(); ++j) {
            char c = s[j];
            if (c == '(' || c == '{' || c == '[') {
                auto e = match(j);
                if (e == std::string::npos) return e;
                j = e - 1;
                continue;
            }
            if (c == '"' || c == '\'') {
                std::size_t k = j + 1;
                while (k < s.size() && s[k] != c) {
                    if (s[k] == '\\') ++k;
                    ++k;
                }
                j = k;
                continue;
            }
            if (c == ';') return j + 1;
        }
        return std::string::npos;
    }
    bool fail(std::string m) {
        if (why.empty()) why = std::move(m);
        return false;
    }
    std::string inner_of(std::size_t a, std::size_t b) const {  // statement text, braces stripped
        auto t = trim(s.substr(a, b - a));
        if (t.size() >= 2 && t.front() == '{' && t.back() == '}') return t.substr(1, t.size() - 2);
        return t;
    }

    bool block(std::size_t i, std::size_t end) {
        while (true) {
            i = skip_ws(i);
            if (i >= end) return true;
            std::size_t next = 0;
            if (!stmt(i, next)) return false;
            if (next == std::string::npos || next <= i) return fail("unparsed statement");
            i = next;
        }
    }

    bool loop_body_ok(const std::string& text) {
        auto tk = lex(text);
        for (std::size_t i = 0; i < tk.size(); ++i) {
            auto& t = tk[i].t;
            if (t == "for" || t == "while" || t == "do") return fail("nested loop");
            if (t == "break" || t == "continue" || t == "goto" || t == "switch" || t == "case")
                return fail("'" + t + "' in loop body");
            if (t == "&" && (i == 0 || !(is_identifier(tk[i - 1].t) || tk[i - 1].t == ")" || tk[i - 1].t == "]")))
                return fail("address-of in loop");
            if (t == "->" || t == ".") return fail("member access in loop");
            if (is_identifier(t) && i + 1 < tk.size() && tk[i + 1].t == "(" && !keywords().count(t) &&
                !type_words().count(t))
                return fail("call to '" + t + "' in loop");
        }
        return true;
    }
    bool cond_ok(const std::string& text) {
        auto tk = lex(text);
        for (auto& t : tk)
            if (is_assign_op(t.t) || t.t == "++" || t.t == "--") return fail("side effect in loop condition");
        return loop_body_ok(text);
    }

    bool stmt(std::size_t i, std::size_t& next) {
        if (s[i] == '{') {
            auto e = match(i);
            if (e == std::string::npos) return fail("unbalanced {");
            if (!block(i + 1, e - 1)) return false;
            next = e;
            return true;
        }
        auto w = word_at(i);
        if (w == "if") {
            auto p = skip_ws(i + 2);
            if (p >= s.size() || s[p] != '(') return fail("if without (");
            auto e = match(p);
            if (e == std::string::npos) return fail("unbalanced (");
            auto t = skip_ws(e);
            std::size_t after = 0;
            if (!stmt(t, after)) return false;
            auto el = skip_ws(after);
            if (word_at(el) == "else") {
                auto t2 = skip_ws(el + 4);
                if (!stmt(t2, after)) return false;
            }
            next = after;
            return true;
        }
        if (w == "for" || w == "while") {
            auto p = skip_ws(i + w.size());
            if (p >= s.size() || s[p] != '(') return fail(w + " without (");
            auto e = match(p);
            if (e == std::string::npos) return fail("unbalanced (");
            auto head = s.substr(p + 1, e - p - 2);
            auto t = skip_ws(e);
            std::size_t after = 0;
            if (t < s.size() && s[t] == '{') after = match(t);
            else if (t < s.size() && s[t] == ';') after = t + 1;
            else after = simple_end(t);
            if (after == std::string::npos) return fail("unparsed loop body");
            LoopCut L;
            L.kind = w;
            L.begin = i;
            L.end = after;
            L.body = s[t] == ';' ? std::string() : inner_of(t, after);
            if (w == "for") {
                std::vector<std::string> parts;
                int depth = 0;
                std::string cur;
                for (char c : head) {
                    if (c == '(') ++depth;
                    if (c == ')') --depth;
                    if (c == ';' && depth == 0) {
                        parts.push_back(cur);
                        cur.clear();
                    } else {
                        cur += c;
                    }
                }
                parts.push_back(cur);
                if (parts.size() != 3) return fail("for header");
                L.init = trim(parts[0]);
                L.cond = trim(parts[1]);
                L.incr = trim(parts[2]);
            } else {
                L.cond = trim(head);
            }
            if (L.cond.empty()) L.cond = "1";
            if (!loop_body_ok(L.body) || !cond_ok(L.cond) || !loop_body_ok(L.incr)) return false;
            loops.push_back(std::move(L));
            next = after;
            return true;
        }
        if (w == "do") {
            auto t = skip_ws(i + 2);
            std::size_t after = 0;
            if (t < s.size() && s[t] == '{') after = match(t);
            else after = simple_end(t);
            if (after == std::string::npos) return fail("unparsed do body");
            auto wpos = skip_ws(after);
            if (word_at(wpos) != "while") return fail("do without while");
            auto p = skip_ws(wpos + 5);
            if (p >= s.size() || s[p] != '(') return fail("while without (");
            auto e = match(p);
            if (e == std::string::npos) return fail("unbalanced (");
            auto semi = skip_ws(e);
            if (semi >= s.size() || s[semi] != ';') return fail("do-while without ;");
            LoopCut L;
            L.kind = "do";
            L.begin = i;
            L.end = semi + 1;
            L.body = inner_of(t, after);
            L.cond = trim(s.substr(p + 1, e - p - 2));
            if (!loop_body_ok(L.body) || !cond_ok(L.cond)) return false;
            loops.push_back(std::move(L));
            next = semi + 1;
            return true;
        }
        if (w == "switch") {
            auto p = skip_ws(i + 6);
            if (p >= s.size() || s[p] != '(') return fail("switch without (");
            auto e = match(p);
            auto t = skip_ws(e);
            if (t >= s.size() || s[t] != '{') return fail("switch body");
            auto after = match(t);
            if (after == std::string::npos) return fail("unbalanced switch");
            auto inner = s.substr(t, after - t);
            for (auto& tk : lex(inner))
                if (tk.t == "for" || tk.t == "while" || tk.t == "do") return fail("loop inside switch");
            next = after;
            return true;
        }
        if (w == "else") return fail("dangling else");
        auto e = simple_end(i);
        if (e == std::string::npos) return fail("statement without ;");
        next = e;
        return true;
    }
};

std::vector<long long> literals(const std::string& text) {
    std::set<long long> vals;
    for (auto& t : lex(text)) {
        if (t.t.empty() || !std::isdigit(static_cast<unsigned char>(t.t[0]))) continue;
        try {
            std::size_t used = 0;
            auto v = std::stoll(t.t, &used, 0);
            if (std::llabs(v) <= 1000000) vals.insert(v);
        } catch (...) {
        }
    }
    vals.insert(0);
    vals.insert(1);
    std::vector<long long> out(vals.begin(), vals.end());
    if (out.size() > 10) out.resize(10);
    return out;
}

bool has_pointer_decl(const std::string& body) {
    auto tk = lex(body);
    for (std::size_t i = 1; i < tk.size(); ++i) {
        if (tk[i].t != "*") continue;
        auto& prev = tk[i - 1].t;
        if (type_words().count(prev) || prev == "*" || (prev.size() > 2 && prev.ends_with("_t")))
            return true;
    }
    return false;
}

}  // namespace

std::optional<std::vector<LoopCut>> loop_cuts(const FunctionInfo& fn, std::string* why) {
    auto say = [&](std::string m) -> std::optional<std::vector<LoopCut>> {
        if (why) *why = std::move(m);
        return std::nullopt;
    };
    if (has_pointer_decl(fn.body)) return say("pointer declaration (aliasing is not havocked soundly)");
    for (auto& t : lex(fn.body))
        if (t.t == "goto" || t.t == "setjmp" || t.t == "longjmp") return say("'" + t.t + "' in function");
    Walker w(fn.body);
    if (!w.block(0, fn.body.size())) return say(w.why.empty() ? "unparsed body" : w.why);
    // Vocabulary: scalar params + scalars declared before the loop (or in its
    // for-init), minus names declared inside the body.
    std::vector<std::string> param_scalars;
    std::vector<std::pair<std::string, int>> param_arrays;
    for (auto& [typ, name] : fn.params) {
        if (name.empty()) continue;
        if (typ.find('*') != std::string::npos || typ.find('[') != std::string::npos) continue;
        param_scalars.push_back(name);
    }
    auto consts = literals(fn.body);
    for (auto& L : w.loops) {
        std::vector<std::string> before_scalars;
        std::vector<std::pair<std::string, int>> before_arrays;
        decls_of(lex(fn.body.substr(0, L.begin)), before_scalars, before_arrays);
        decls_of(lex(L.init + ";"), before_scalars, before_arrays);
        std::vector<std::string> body_scalars;
        std::vector<std::pair<std::string, int>> body_arrays;
        decls_of(lex(L.body), body_scalars, body_arrays);
        std::set<std::string> inner(body_scalars.begin(), body_scalars.end());
        for (auto& [n, _] : body_arrays) inner.insert(n);
        std::set<std::string> vocab_set;
        std::vector<std::string> vocab;
        auto add = [&](const std::string& n) {
            if (inner.count(n) || vocab_set.count(n)) return;
            for (auto& [an, _] : before_arrays)
                if (an == n) return;
            vocab_set.insert(n);
            vocab.push_back(n);
        };
        for (auto& n : param_scalars) add(n);
        for (auto& n : before_scalars) add(n);
        auto wr = writes_of(lex(L.body + "\n" + L.incr + ";"));
        std::set<std::string> hv;
        for (auto& n : wr.scalars)
            if (!inner.count(n)) hv.insert(n);
        for (auto& n : wr.arrays) hv.insert(n);
        L.havoc.assign(hv.begin(), hv.end());
        L.scalars = vocab;
        for (auto& n : vocab)
            if (wr.scalars.count(n)) L.modified.push_back(n);
        L.arrays = before_arrays;
        for (auto& a : param_arrays) L.arrays.push_back(a);
        L.constants = consts;
        // A written name that is neither in scope nor declared in the body
        // (a global) is havocked as well; that is already in hv.
    }
    return w.loops;
}

std::vector<std::string> template_candidates(const FunctionInfo& fn, const LoopCut& L) {
    (void)fn;
    std::vector<std::string> out;
    std::set<std::string> seen;
    auto push = [&](const std::string& e) {
        if (out.size() >= 240) return;
        if (!valid_c_bool_expr(e, [&] {
                auto v = L.scalars;
                return v;
            }()))
            return;
        if (seen.insert(e).second) out.push_back(e);
    };
    std::set<std::string> vocab(L.scalars.begin(), L.scalars.end());
    // (1) bounds from the loop condition atoms.
    {
        std::vector<std::string> atoms;
        int depth = 0;
        std::string cur;
        for (std::size_t i = 0; i < L.cond.size(); ++i) {
            char c = L.cond[i];
            if (c == '(') ++depth;
            if (c == ')') --depth;
            if (depth == 0 && L.cond.compare(i, 2, "&&") == 0) {
                atoms.push_back(cur);
                cur.clear();
                ++i;
                continue;
            }
            cur += c;
        }
        atoms.push_back(cur);
        static const char* rel[] = {"<=", ">=", "!=", "<", ">"};
        for (auto a : atoms) {
            a = trim(a);
            while (a.size() > 2 && a.front() == '(' && a.back() == ')') a = trim(a.substr(1, a.size() - 2));
            for (auto* r : rel) {
                auto p = a.find(r);
                if (p == std::string::npos) continue;
                auto lhs = trim(a.substr(0, p));
                auto rhs = trim(a.substr(p + std::strlen(r)));
                std::string op = r;
                if (vocab.count(lhs)) {
                    if (op == "<") push(lhs + " <= " + rhs);
                    if (op == "<=") push(lhs + " <= " + rhs + " + 1");
                    if (op == ">") push(lhs + " >= " + rhs);
                    if (op == ">=") push(lhs + " >= " + rhs + " - 1");
                    if (op == "!=") {
                        push(lhs + " <= " + rhs);
                        push(lhs + " >= " + rhs);
                    }
                }
                if (vocab.count(rhs)) {
                    if (op == "<") push(rhs + " >= " + lhs);
                    if (op == "<=") push(rhs + " >= " + lhs + " - 1");
                    if (op == ">") push(rhs + " <= " + lhs);
                    if (op == ">=") push(rhs + " <= " + lhs + " + 1");
                    if (op == "!=") {
                        push(rhs + " <= " + lhs);
                        push(rhs + " >= " + lhs);
                    }
                }
                break;
            }
        }
    }
    // (2) sign facts of written scalars.
    for (auto& x : L.modified) {
        push(x + " >= 0");
        push(x + " > 0");
        push(x + " <= 0");
    }
    // (3) array bounds for written counters.
    for (auto& x : L.modified)
        for (auto& [a, n] : L.arrays) {
            push(x + " <= " + std::to_string(n));
            push(x + " < " + std::to_string(n));
        }
    // (4) constant bounds.
    for (auto& x : L.modified)
        for (auto k : L.constants) {
            push(x + " <= " + std::to_string(k));
            push(x + " >= " + std::to_string(k));
        }
    // (5) order relations with every scalar in scope.
    for (auto& x : L.modified)
        for (auto& y : L.scalars) {
            if (x == y) continue;
            push(x + " <= " + y);
            push(x + " >= " + y);
            push(x + " == " + y);
            push(x + " < " + y);
        }
    // (6) counter/accumulator: small linear relations over written pairs and
    //     written-vs-parameter pairs.
    std::vector<long long> coeffs{2, 3, 4};
    for (auto k : L.constants)
        if (k > 4 && k <= 1000) coeffs.push_back(k);
    for (auto& x : L.modified)
        for (auto& y : L.scalars) {
            if (x == y) continue;
            for (auto c : coeffs) {
                push(x + " <= " + std::to_string(c) + " * " + y);
                push(x + " >= " + std::to_string(c) + " * " + y);
                push(x + " == " + std::to_string(c) + " * " + y);
            }
            for (auto k : L.constants) {
                if (k == 0) continue;
                push(x + " == " + y + " + " + std::to_string(k));
                push(x + " + " + y + " == " + std::to_string(k));
            }
        }
    return out;
}

std::string cut_program(const FunctionInfo& fn, const std::vector<LoopCut>& loops,
                        const std::vector<std::vector<std::pair<int, std::string>>>& inv) {
    std::string body = fn.body;
    for (std::size_t j = loops.size(); j-- > 0;) {
        auto& L = loops[j];
        const auto& I = j < inv.size() ? inv[j] : std::vector<std::pair<int, std::string>>{};
        std::string asserts, assume;
        for (auto& [tag, e] : I) {
            asserts += "__prism_assert(" + std::to_string(tag) + ", (" + e + "));\n";
            if (!assume.empty()) assume += " && ";
            assume += "(" + e + ")";
        }
        std::string havoc;
        for (auto& v : L.havoc) havoc += "__prism_havoc(" + v + ");\n";
        std::string cut = "{\n";
        if (!L.init.empty()) cut += L.init + ";\n";
        cut += asserts + havoc;
        if (!assume.empty()) cut += "__prism_assume(" + assume + ");\n";
        if (L.kind == "do") {
            cut += "{\n" + L.body + "\n}\n";
            cut += "__prism_step (" + L.cond + ") {\n" + asserts + "}\n";
        } else {
            cut += "__prism_step (" + L.cond + ") {\n{\n" + L.body + "\n}\n";
            if (!L.incr.empty()) cut += L.incr + ";\n";
            cut += asserts + "}\n";
        }
        cut += "}\n";
        body = body.substr(0, L.begin) + cut + body.substr(L.end);
    }
    return body;
}

HoudiniResult houdini(const FunctionInfo& fn, const std::vector<LoopCut>& loops,
                      std::vector<std::vector<std::string>> candidates,
                      std::vector<std::vector<std::string>> sources, int unwind) {
    HoudiniResult h;
    candidates.resize(loops.size());
    sources.resize(loops.size());
    struct Cand {
        std::size_t loop;
        std::string expr, source;
        bool alive = true;
    };
    std::vector<Cand> all;
    for (std::size_t j = 0; j < loops.size(); ++j)
        for (std::size_t k = 0; k < candidates[j].size(); ++k)
            all.push_back({j, candidates[j][k], k < sources[j].size() ? sources[j][k] : "template"});
    auto build = [&] {
        std::vector<std::vector<std::pair<int, std::string>>> inv(loops.size());
        for (std::size_t t = 0; t < all.size(); ++t)
            if (all[t].alive) inv[all[t].loop].push_back({static_cast<int>(t), all[t].expr});
        return cut_program(fn, loops, inv);
    };
    for (int round = 0; round < 64; ++round) {
        h.rounds = round + 1;
        bool any_alive = false;
        for (auto& c : all) any_alive |= c.alive;
        if (!any_alive) break;
        auto pc = check_program(fn, build(), unwind, 4000, /*only_invariants=*/true);
        if (!pc.encoded) {
            h.why = "cut program not encodable: " + pc.error;
            return h;
        }
        bool dropped = false;
        for (auto& p : pc.props) {
            if (p.name.rfind("ai-inv#", 0) != 0) continue;
            if (p.result == "unsat") continue;
            int tag = -1;
            try {
                tag = std::stoi(p.name.substr(7));
            } catch (...) {
            }
            if (tag >= 0 && tag < static_cast<int>(all.size()) && all[static_cast<std::size_t>(tag)].alive) {
                all[static_cast<std::size_t>(tag)].alive = false;
                dropped = true;
            }
        }
        if (!dropped) break;
    }
    auto program = build();
    auto fin = check_program(fn, program, unwind, 8000, false);
    h.encoded = fin.encoded;
    h.invariants.assign(loops.size(), {});
    h.sources.assign(loops.size(), {});
    for (auto& c : all)
        if (c.alive) {
            h.invariants[c.loop].push_back(c.expr);
            h.sources[c.loop].push_back(c.source);
        }
    if (!fin.encoded) {
        h.why = "cut program not encodable: " + fin.error;
        return h;
    }
    bool open = false;
    for (auto& p : fin.props) {
        if (p.result == "unsat") continue;
        open = true;
        if (h.cti.empty())
            h.cti = p.name + " (" + p.cls + ") " + p.result + (p.model.empty() ? "" : ": " + p.model);
    }
    if (!fin.unwind_ok) {
        open = true;
        if (h.why.empty()) h.why = "a loop was not cut (unrolling left open)";
    }
    h.proved = !open;
    if (open && h.why.empty()) h.why = "strengthened step or base case open: " + h.cti;
    return h;
}

namespace {

std::string inv_json(const std::vector<std::vector<std::string>>& inv) {
    nlohmann::json j = nlohmann::json::array();
    for (auto& l : inv) j.push_back(l);
    return j.dump();
}

Finding proved_finding(const FunctionInfo& fn, const Finding& bounded, const HoudiniResult& h,
                       const std::string& source) {
    Finding f;
    f.stage = "bmc";
    f.status = std::string(laws::PROVED_UNBOUNDED);
    f.file = fn.file;
    f.function = fn.name;
    f.line = fn.line;
    f.strength = std::string(laws::STRENGTH_PROVES);
    f.extra = bounded.extra;
    f.extra["k_induction"] = "closed-invariants";
    f.extra["unwind_closed"] = "false";
    f.extra["bounded_status"] = bounded.status;
    f.extra["invariants"] = inv_json(h.invariants);
    f.extra["invariant_source"] = source;
    f.extra["houdini_rounds"] = std::to_string(h.rounds);
    f.extra["invariant_checker"] = "z3: Houdini filter + loop-cut induction (base and step)";
    std::size_t n = 0;
    for (auto& l : h.invariants) n += l.size();
    f.message = "k-induction step closed with " + std::to_string(n) + " Houdini invariant(s) (" + source +
                "); base case holds; not a bounded-only result";
    return f;
}

}  // namespace

Finding strengthen_bounded(const FunctionInfo& fn, const Finding& bounded, int unwind) {
    Finding rec = bounded;
    std::string why;
    auto loops = loop_cuts(fn, &why);
    if (!loops) {
        rec.extra["invariants_attempt"] = "refused: " + why;
        return rec;
    }
    if (loops->empty()) {
        rec.extra["invariants_attempt"] = "no loop to cut";
        return rec;
    }
    std::vector<std::vector<std::string>> cands, srcs;
    for (auto& L : *loops) {
        cands.push_back(template_candidates(fn, L));
        srcs.push_back(std::vector<std::string>(cands.back().size(), "template"));
    }
    auto h = houdini(fn, *loops, cands, srcs, unwind);
    if (h.proved) return proved_finding(fn, bounded, h, "template");
    rec.extra["invariants_attempt"] = h.why.empty() ? std::string("open") : h.why;
    rec.extra["invariants"] = inv_json(h.invariants);
    if (!h.encoded) return rec;

    // Model-assisted rounds: the counterexample to induction goes back to the
    // model, which proposes more candidates; Houdini decides again.
    std::string unavailable;
    auto backend = session_backend(&unavailable);
    if (!backend) {
        rec.extra["llm_invariants"] = "NOTRUN: " + unavailable;
        return rec;
    }
    const int budget = 3;
    std::string cti = h.cti;
    std::string src = function_source(fn);
    for (int round = 0; round < budget; ++round) {
        std::vector<std::string> vocab;
        for (auto& L : *loops)
            for (auto& v : L.scalars)
                if (std::find(vocab.begin(), vocab.end(), v) == vocab.end()) vocab.push_back(v);
        ModelRequest req;
        req.feature = "invariants";
        req.grammar = "invariants";
        req.grammar_text = grammar_for("invariants", vocab);
        req.system = system_prompt(
            "Task: propose loop invariants (C boolean expressions over the listed variables) that hold at "
            "the head of every loop of the function and make its undefined-behaviour checks provable by "
            "induction. Reply with a JSON list of expressions only.");
        std::string surv;
        for (auto& l : h.invariants)
            for (auto& e : l) surv += "  " + e + "\n";
        req.user = "Variables: " + join(vocab, ", ") + "\nSurviving invariants so far:\n" +
                   (surv.empty() ? "  (none)\n" : surv) + "Counterexample to induction: " + cti + "\n" +
                   fence_untrusted(src, "SOURCE");
        AuditRecord ar;
        ar.function = fn.name;
        ar.file = fn.file;
        auto reply = ask(*backend, req, ar);
        if (!reply.error.empty()) {
            ar.checker = "none";
            ar.checker_result = "";
            audit_append(ar);
            rec.extra["llm_invariants"] = "ERROR: " + reply.error;
            return rec;
        }
        auto v = validate_invariants(reply.text, vocab);
        if (!v.ok) {
            ar.output_valid = false;
            ar.rejected_reason = v.reason;
            ar.checker = "grammar-validator";
            ar.checker_result = "rejected";
            audit_append(ar);
            rec.extra["llm_invariants"] = "rejected output: " + v.reason;
            rec.extra["ai_audit_id"] = ar.id;
            continue;
        }
        ar.output_valid = true;
        std::string tagsrc = "llm:" + backend->name();
        for (std::size_t j = 0; j < loops->size(); ++j)
            for (auto& e : v.items) {
                if (std::find(cands[j].begin(), cands[j].end(), e) != cands[j].end()) continue;
                // A candidate over a name not in this loop's scope is kept out.
                if (!valid_c_bool_expr(e, (*loops)[j].scalars)) continue;
                cands[j].push_back(e);
                srcs[j].push_back(tagsrc);
            }
        h = houdini(fn, *loops, cands, srcs, unwind);
        bool llm_survivor = false;
        for (auto& l : h.sources)
            for (auto& s : l) llm_survivor |= s != "template";
        ar.checker = "z3-houdini+k-induction";
        ar.checker_result = h.proved ? std::string(laws::PROVED_UNBOUNDED) : "step-open";
        ar.verdict_effect = (h.proved && llm_survivor) ? std::string(laws::PROVED_UNBOUNDED) : "none";
        audit_append(ar);
        if (h.proved) {
            auto f = proved_finding(fn, bounded, h, llm_survivor ? tagsrc : "template");
            f.extra["ai_audit_id"] = ar.id;
            f.extra["ai_checker"] = ar.checker;
            f.extra["ai_checker_result"] = ar.checker_result;
            return f;
        }
        rec.extra["llm_invariants"] = "step open after model round " + std::to_string(round + 1);
        rec.extra["ai_audit_id"] = ar.id;
        rec.extra["invariants"] = inv_json(h.invariants);
        cti = h.cti;
    }
    return rec;
}

}  // namespace prism::ai
