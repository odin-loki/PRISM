// Stage contracts: requires/ensures/invariant/decreases comments proved by
// the PIR checker (prove_contracts, prove_with_contract; check_with_assume is shared with wp).
#include "common.hpp"

namespace prism {
namespace fs = std::filesystem;
using namespace stages_detail;

namespace {
std::pair<std::string, std::string> take_block_keep_braces(std::string text) {
    text = lstrip(text);
    if (text.starts_with("{")) {
        auto [inner, rest] = brace(text);
        return {"{" + inner + "}", rest};
    }
    return stmt(text);
}

bool has_loop(const std::string& body) { return re_search("\\b(while|for)\\b", body); }

bool encode_decreases(const std::string& expr) {
    // A linear integer measure: identifiers and decimal constants joined by
    // binary + / - (`i`, `n - i`, `hi - lo + 1`). wrap_loop_dec only adds
    // asserts (measure >= 0 at entry, strictly smaller after the body), and
    // the checker checks signed overflow in evaluating it, so an overflowing or
    // non-decreasing measure is FAILED, never assumed; accepting a measure
    // can refute, it cannot prove anything. `*`, calls, products, unary
    // minus and tuples stay ERROR (decreases_unencoded).
    static Regex re("^(?:[A-Za-z_]\\w*|0|[1-9]\\d{0,8})(?:[+-](?:[A-Za-z_]\\w*|0|[1-9]\\d{0,8}))*$");
    std::string compact;
    for (char c : expr)
        if (!std::isspace(static_cast<unsigned char>(c))) compact.push_back(c);
    return !compact.empty() && fullmatch(re, compact);
}

bool encode_invariant(const std::string& expr) {
    static Regex re("^[A-Za-z_]\\w*\\s*(<=|>=|<|>|==|!=)\\s*([A-Za-z_]\\w*|0|[1-9]\\d*)$");
    auto parts = split_comma(expr); // wrong - split on &&
    std::vector<std::string> ps;
    std::string cur;
    for (std::size_t i = 0; i < expr.size();) {
        if (i + 1 < expr.size() && expr.at(i) == '&' && expr[i + 1] == '&') {
            auto p = strip(cur);
            if (!p.empty()) ps.push_back(p);
            cur.clear();
            i += 2;
        } else {
            cur.push_back(expr.at(i++));
        }
    }
    auto tail = strip(cur);
    if (!tail.empty()) ps.push_back(tail);
    if (ps.empty()) return false;
    for (auto& p : ps)
        if (p.empty() || !fullmatch(re, p)) return false;
    return true;
}

std::string wrap_loop_inv(std::string loop_body, const std::string& inv) {
    auto inner = strip(loop_body);
    if (inner.starts_with("{") && inner.ends_with("}")) inner = inner.substr(1, inner.size() - 2);
    else inner = loop_body;
    return "{ { assert(" + inv + "); } " + inner + " { assert(" + inv + "); } }";
}

std::string wrap_loop_dec(std::string loop_body, const std::string& dec, int vid) {
    auto vn = "__prism_d" + std::to_string(vid);
    auto inner = strip(loop_body);
    if (inner.starts_with("{") && inner.ends_with("}")) inner = inner.substr(1, inner.size() - 2);
    else inner = loop_body;
    return "{ { assert((" + dec + ") >= 0); } { int " + vn + " = (" + dec + "); " + inner + " assert((" + dec +
           ") < " + vn + "); } }";
}

std::string transform_stmt_stream(const std::string& text_in,
                                  const std::function<std::string(const std::string&, const std::string&,
                                                                 const std::string&)>& on_loop,
                                  bool& ok) {
    std::string out;
    std::string text = text_in;
    std::size_t i = 0;
    try {
        while (i < text.size()) {
            auto rest = text.substr(i);
            auto stripped = lstrip(rest);
            if (stripped.empty()) {
                out += rest;
                break;
            }
            auto pad = rest.substr(0, rest.size() - stripped.size());
            out += pad;
            i += pad.size();
            if (stripped.starts_with("{")) {
                auto [inner, after] = brace(stripped);
                out += "{" + transform_stmt_stream(inner, on_loop, ok) + "}";
                i += stripped.size() - after.size();
                continue;
            }
            bool matched = false;
            for (auto* kw : {"while", "for"}) {
                if (starts_kw(stripped, kw)) {
                    auto [header, after_paren] = paren(stripped.substr(std::strlen(kw)));
                    auto [loop_body, after_body] = take_block_keep_braces(after_paren);
                    out += on_loop(kw, header, loop_body);
                    i += stripped.size() - after_body.size();
                    matched = true;
                    break;
                }
            }
            if (matched) continue;
            if (starts_kw(stripped, "if")) {
                auto [cond, after_paren] = paren(stripped.substr(2));
                auto [then_src, after_then] = take_block_keep_braces(after_paren);
                after_then = lstrip(after_then);
                std::string clause = "if (" + cond + ") " + transform_stmt_stream(then_src, on_loop, ok);
                if (starts_kw(after_then, "else")) {
                    auto [else_src, after_else] = take_block_keep_braces(after_then.substr(4));
                    clause += " else " + transform_stmt_stream(else_src, on_loop, ok);
                    after_then = after_else;
                }
                out += clause;
                i += stripped.size() - after_then.size();
                continue;
            }
            auto [stt, after] = take_block_keep_braces(stripped);
            out += stt;
            i += stripped.size() - after.size();
        }
    } catch (const ParseFail&) {
        ok = false;
        return text_in;
    } catch (const std::exception&) {
        ok = false;
        return text_in;
    }
    return out;
}

std::pair<std::string, bool> instrument_invariant(const std::string& body, const std::string& invariant) {
    auto inv = strip(invariant);
    if (!encode_invariant(inv)) return {body, false};
    bool ok = true;
    auto out = transform_stmt_stream(body,
                                     [&](const std::string& kw, const std::string& header, const std::string& lb) {
                                         return "{ assert(" + inv + "); } " + kw + " (" + header + ") " +
                                                wrap_loop_inv(lb, inv);
                                     },
                                     ok);
    return {ok ? out : body, ok};
}

std::pair<std::string, bool> instrument_decreases(const std::string& body, const std::string& decreases) {
    auto dec = strip(decreases);
    if (!encode_decreases(dec)) return {body, false};
    int vid = 0;
    bool ok = true;
    auto out = transform_stmt_stream(body,
                                     [&](const std::string& kw, const std::string& header, const std::string& lb) {
                                         ++vid;
                                         return kw + " (" + header + ") " + wrap_loop_dec(lb, dec, vid);
                                     },
                                     ok);
    return {ok ? out : body, ok};
}

// Every `return` is matched, including `return(x);` and `return;`: a return
// the regex missed would leave its path without the ensures assert, and the
// the checker would prove a contract that path breaks.
const Regex& return_stmt_re() {
    static Regex re("\\breturn\\b\\s*([^;]*);");
    return re;
}

bool has_empty_return(const std::string& body) {
    for (auto& m : return_stmt_re().finditer(body))
        if (strip(m.group(1)).empty()) return true;
    return false;
}

// ---- ACSL clause text -> a C expression the checker asserts ----------------
//
// Clauses are C expressions with these ACSL forms (docs/CONTRACTS.md):
//   \result, \old(e) (ensures only: e is evaluated once on entry, after the
//   requires), A ==> B, A <==> B, chained comparisons (0 <= i < n means
//   0 <= i && i < n, never C's (0 <= i) < n), \true, \false, \null, and in a
//   requires \valid(...), \valid_read(...), \separated(...) (the pir stage
//   binds the objects they name and lists that as an assumption; in the C
//   text they are 1). A formal parameter in an ensures denotes its value on
//   entry (ACSL), not after the body changed it. Any other \construct
//   (\forall, \exists, \at, \let, ...) is refused: ERROR, never a proof.
struct AcslC {
    std::string error;                                      // non-empty: not encoded
    std::vector<std::pair<std::string, std::string>> olds;  // __prism_old_k, entry expression
    std::set<std::string> pre_params;                       // parameters an ensures reads
    int next_old = 0;
};

bool acsl_ident_char(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }

// Index of the first character after a char/string literal opening at s[i].
std::size_t acsl_skip_literal(const std::string& s, std::size_t i) {
    const char q = s[i];
    for (++i; i < s.size() && s[i] != q; ++i)
        if (s[i] == '\\') ++i;
    return std::min(s.size(), i + 1);
}

// The index just after the group opened at s[open] ('(' or '['), npos if unbalanced.
std::size_t acsl_close(const std::string& s, std::size_t open) {
    int depth = 0;
    for (std::size_t i = open; i < s.size();) {
        char c = s[i];
        if (c == '\'' || c == '"') {
            i = acsl_skip_literal(s, i);
            continue;
        }
        if (c == '(' || c == '[' || c == '{') ++depth;
        else if ((c == ')' || c == ']' || c == '}') && --depth == 0)
            return i + 1;
        ++i;
    }
    return std::string::npos;
}

// Top-level pieces of s split at any of ops (longest first), with the separators.
std::pair<std::vector<std::string>, std::vector<std::string>> acsl_split(const std::string& s,
                                                                         const std::vector<std::string>& ops) {
    std::vector<std::string> pieces, seps;
    std::string cur;
    int depth = 0;
    for (std::size_t i = 0; i < s.size();) {
        char c = s[i];
        if (c == '\'' || c == '"') {
            auto j = acsl_skip_literal(s, i);
            cur.append(s, i, j - i);
            i = j;
            continue;
        }
        if (c == '(' || c == '[' || c == '{') ++depth;
        if (c == ')' || c == ']' || c == '}') --depth;
        bool hit = false;
        if (depth == 0)
            for (auto& op : ops)
                if (s.compare(i, op.size(), op) == 0) {
                    pieces.push_back(cur);
                    seps.push_back(op);
                    cur.clear();
                    i += op.size();
                    hit = true;
                    break;
                }
        if (!hit) cur.push_back(s[i++]);
    }
    pieces.push_back(cur);
    return {pieces, seps};
}

// Comparison operators at depth 0 of s: (position, operator); << >> -> are not.
std::vector<std::pair<std::size_t, std::string>> acsl_comparisons(const std::string& s) {
    std::vector<std::pair<std::size_t, std::string>> out;
    int depth = 0;
    for (std::size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (c == '\'' || c == '"') {
            i = acsl_skip_literal(s, i) - 1;
            continue;
        }
        if (c == '(' || c == '[' || c == '{') ++depth;
        if (c == ')' || c == ']' || c == '}') --depth;
        if (depth != 0) continue;
        const char nx = i + 1 < s.size() ? s[i + 1] : '\0';
        if ((c == '<' && nx == '<') || (c == '>' && nx == '>')) {
            ++i;
            continue;
        }
        if (c == '>' && i > 0 && s[i - 1] == '-') continue;  // ->
        if ((c == '<' || c == '>' || c == '=' || c == '!') && nx == '=') {
            out.emplace_back(i, std::string{c, '='});
            ++i;
        } else if (c == '<' || c == '>') {
            out.emplace_back(i, std::string(1, c));
        }
    }
    return out;
}

std::string acsl_conv(const std::string& s, AcslC& st);

// Paren and bracket groups of a piece, converted recursively.
std::string acsl_groups(const std::string& s, AcslC& st) {
    std::string out;
    for (std::size_t i = 0; i < s.size();) {
        char c = s[i];
        if (c == '\'' || c == '"') {
            auto j = acsl_skip_literal(s, i);
            out.append(s, i, j - i);
            i = j;
            continue;
        }
        if (c == '(' || c == '[') {
            auto e = acsl_close(s, i);
            if (e == std::string::npos) {
                st.error = "unbalanced brackets";
                return s;
            }
            out.push_back(c);
            out += acsl_conv(s.substr(i + 1, e - i - 2), st);
            out.push_back(s[e - 1]);
            i = e;
            continue;
        }
        out.push_back(s[i++]);
    }
    return out;
}

std::string acsl_conv(const std::string& s, AcslC& st) {
    auto [imp, isep] = acsl_split(s, {"<==>", "==>"});
    if (imp.size() > 1) {
        // right-associative, below || in precedence
        std::string r = acsl_conv(imp.back(), st);
        for (std::size_t k = imp.size() - 1; k-- > 0;) {
            auto a = acsl_conv(imp[k], st);
            r = isep[k] == "==>" ? "(!(" + a + ") || (" + r + "))" : "(!(" + a + ") == !(" + r + "))";
        }
        return r;
    }
    // Comparisons bind tighter than these, so a chain never spans one of them.
    auto [pieces, seps] = acsl_split(s, {"&&", "||", "?", ":", ",", "&", "|", "^"});
    std::string out;
    for (std::size_t k = 0; k < pieces.size(); ++k) {
        auto p = acsl_groups(pieces[k], st);
        auto cmps = acsl_comparisons(p);
        if (cmps.size() >= 2) {
            std::vector<std::string> terms;
            std::size_t from = 0;
            for (auto& [pos, op] : cmps) {
                terms.push_back(strip(p.substr(from, pos - from)));
                from = pos + op.size();
            }
            terms.push_back(strip(p.substr(from)));
            std::string chain;
            for (std::size_t j = 0; j < cmps.size(); ++j)
                chain += std::string(j ? " && " : "") + "(" + terms[j] + " " + cmps[j].second + " " + terms[j + 1] + ")";
            p = " (" + chain + ") ";
        }
        out += p;
        if (k < seps.size()) out += seps[k];
    }
    return out;
}

// Replace the \name constructs; `pre`: a requires (validity atoms allowed, \old not).
std::string acsl_backslash(const std::string& s, bool pre, AcslC& st) {
    std::string out;
    for (std::size_t i = 0; i < s.size();) {
        if (s[i] == '\'' || s[i] == '"') {
            auto j = acsl_skip_literal(s, i);
            out.append(s, i, j - i);
            i = j;
            continue;
        }
        if (s[i] != '\\') {
            out.push_back(s[i++]);
            continue;
        }
        std::size_t j = i + 1;
        while (j < s.size() && acsl_ident_char(s[j])) ++j;
        auto name = s.substr(i + 1, j - i - 1);
        if (name == "true" || name == "false" || name == "null" || name == "result") {
            out += name == "true" ? "1" : name == "false" ? "0" : name == "null" ? "((void *)0)" : "result";
            i = j;
            continue;
        }
        std::size_t k = j;
        while (k < s.size() && std::isspace(static_cast<unsigned char>(s[k]))) ++k;
        const bool call = k < s.size() && s[k] == '(';
        auto e = call ? acsl_close(s, k) : std::string::npos;
        if ((name == "valid" || name == "valid_read" || name == "separated") && e != std::string::npos) {
            if (!pre) {
                st.error = "\\" + name + " in an ensures is not encoded";
                return s;
            }
            out += "1";
            i = e;
            continue;
        }
        if (name == "old" && e != std::string::npos) {
            if (pre) {
                st.error = "\\old in a requires";
                return s;
            }
            AcslC inner;
            auto x = acsl_conv(acsl_backslash(s.substr(k + 1, e - k - 2), true, inner), inner);
            if (!inner.error.empty() || re_search("\\bresult\\b", x)) {
                st.error = inner.error.empty() ? "\\old of \\result" : inner.error;
                return s;
            }
            auto v = "__prism_old_" + std::to_string(st.next_old++);
            st.olds.emplace_back(v, x);
            out += v;
            i = e;
            continue;
        }
        st.error = "ACSL construct \\" + name + " is not encoded";
        return s;
    }
    return out;
}

// result -> __prism_result; a parameter -> its value on entry (__prism_pre_<p>).
std::string acsl_post_names(const std::string& s, const FunctionInfo& fn, AcslC& st) {
    std::set<std::string> params;
    for (auto& [_, n] : fn.params) params.insert(n);
    std::string out;
    for (std::size_t i = 0; i < s.size();) {
        if (s[i] == '\'' || s[i] == '"') {
            auto j = acsl_skip_literal(s, i);
            out.append(s, i, j - i);
            i = j;
            continue;
        }
        if (!acsl_ident_char(s[i]) || std::isdigit(static_cast<unsigned char>(s[i]))) {
            // a number (0x1f, 10u) is one token: copy it whole
            std::size_t j = i + 1;
            if (std::isdigit(static_cast<unsigned char>(s[i])))
                while (j < s.size() && acsl_ident_char(s[j])) ++j;
            out.append(s, i, j - i);
            i = j;
            continue;
        }
        std::size_t j = i;
        while (j < s.size() && acsl_ident_char(s[j])) ++j;
        auto id = s.substr(i, j - i);
        std::size_t b = out.size();
        while (b > 0 && out[b - 1] == ' ') --b;
        const bool member = (b > 0 && out[b - 1] == '.') || (b > 1 && out[b - 1] == '>' && out[b - 2] == '-');
        if (!member && id == "result") {
            out += "__prism_result";
        } else if (!member && params.contains(id)) {
            out += "__prism_pre_" + id;
            st.pre_params.insert(id);
        } else {
            out += id;
        }
        i = j;
    }
    return out;
}

std::optional<std::string> acsl_requires_c(const std::string& r, AcslC& st) {
    auto x = acsl_conv(acsl_backslash(r, true, st), st);
    if (!st.error.empty()) return std::nullopt;
    return x;
}

std::optional<std::string> acsl_ensures_c(const std::string& e, const FunctionInfo& fn, AcslC& st) {
    auto x = acsl_backslash(e, false, st);
    if (!st.error.empty()) return std::nullopt;
    x = acsl_conv(x, st);
    if (!st.error.empty()) return std::nullopt;
    return acsl_post_names(x, fn, st);
}

// fn's return type as C sees it: __typeof__ of a call (not evaluated).
std::string result_type(const FunctionInfo& fn) {
    std::string call = fn.name + "(";
    for (std::size_t i = 0; i < fn.params.size(); ++i) call += (i ? ", " : "") + fn.params[i].second;
    return "__typeof__(" + call + "))";
}

bool returns_void(const FunctionInfo& fn) { return strip(fn.return_type) == "void"; }

std::string rewrite_returns(const std::string& body, const std::string& ensures, const FunctionInfo& fn) {
    auto& re = return_stmt_re();
    std::string out;
    std::size_t i = 0;
    const bool is_void = returns_void(fn);
    for (auto& m : re.finditer(body)) {
        if (m.spans.empty()) continue;
        auto a = static_cast<std::size_t>(std::max(0, m.spans[0].first));
        auto b = static_cast<std::size_t>(std::max(0, m.spans[0].second));
        if (a < i) continue;
        out.append(body, i, a - i);
        auto expr = strip(m.group(1));
        // An empty return in a value-returning function is refused before
        // this (check_with_assume); kept verbatim only so the text stays
        // well-formed.
        if (is_void && expr.empty()) out += "{ assert(" + ensures + "); return; }";
        else if (expr.empty() || is_void) out.append(body, a, b - a);
        else
            out += "{ " + result_type(fn) + " __prism_result = (" + expr + "); assert(" + ensures +
                   "); return __prism_result; }";
        i = b;
    }
    out.append(body, i, std::string::npos);
    return out;
}

std::string instrument_body(const std::string& body, const std::optional<std::string>& requires_,
                            const std::optional<std::string>& ensures, const FunctionInfo& fn, const AcslC& st) {
    auto core = body;
    if (ensures) core = rewrite_returns(body, *ensures, fn);
    std::string parts;
    const bool is_void = returns_void(fn);
    // The marker tells the pir stage (pir/contracts.cpp) that the requires
    // conditions are assumed here, so a violation is not one they exclude.
    if (requires_)
        parts += "// prism: requires assumed (contracts stage)\nif (!(" + *requires_ + ")) return" +
                 std::string(is_void ? "" : " 0") + ";\n";
    if (ensures) {
        for (auto& p : st.pre_params) parts += "__typeof__(" + p + ") __prism_pre_" + p + " = " + p + ";\n";
        for (auto& [v, x] : st.olds) parts += "__typeof__(" + x + ") " + v + " = (" + x + ");\n";
    }
    parts += "{\n" + core + "\n}";
    if (ensures && (is_void || !re_search("\\breturn\\b", body))) parts += "\nassert(" + *ensures + ");";
    return parts;
}

// Every pointer parameter is named by a \valid / \valid_read requires: the
// pir stage binds each to an object of the stated size (docs/PIR.md "Pointer
// parameters"), so checking the contract invents no buffer.
bool pointer_params_bound(const FunctionInfo& fn, const std::vector<std::string>& reqs) {
    std::string all;
    for (auto& r : reqs) all += r + "\n";
    bool any = false;
    for (auto& [ty, name] : fn.params) {
        if (ty.find('*') == std::string::npos && ty.find('[') == std::string::npos) continue;
        any = true;
        Regex re("\\\\valid(?:_read)?\\s*\\(\\s*" + name + "\\b");
        if (!re.search(all)) return false;
    }
    return any;
}

}  // namespace

namespace stages_detail {
Finding check_with_assume(const FunctionInfo& fn, int unwind, const std::optional<std::string>& requires_,
                        const std::optional<std::string>& ensures, const std::optional<std::string>& decreases,
                        const std::optional<std::string>& invariant) {
    auto core = fn.body;
    std::map<std::string, std::string> inv_extra, dec_extra;
    auto fail = [&](std::string msg, std::map<std::string, std::string> extra) {
        auto f = make_find("contracts", laws::ERROR, fn, "FUNC-CONTRACT", std::move(msg), laws::STRENGTH_READS);
        f.extra = std::move(extra);
        if (requires_) f.extra["requires"] = *requires_;
        if (ensures) f.extra["ensures"] = *ensures;
        return f;
    };
    if (invariant && has_loop(core)) {
        if (!encode_invariant(*invariant))
            return fail("cannot encode invariant (" + *invariant + ") for the checker",
                        {{"invariant", *invariant}, {"invariant_unencoded", "true"}});
        bool ok = false;
        std::tie(core, ok) = instrument_invariant(core, *invariant);
        inv_extra["invariant"] = *invariant;
        inv_extra["invariant_encoded"] = ok ? "true" : "false";
        if (!ok)
            return fail("invariant (" + *invariant + ") not instrumented",
                        {{"invariant", *invariant}, {"invariant_unencoded", "true"}});
    }
    if (decreases) {
        // Honesty: a non-identifier measure is ERROR even when there is no
        // loop to instrument. Never silently drop it into PROVED-ASSUMING.
        if (!encode_decreases(*decreases))
            return fail("cannot encode decreases (" + *decreases + ") for the checker",
                        {{"decreases", *decreases}, {"decreases_unencoded", "true"}});
        if (has_loop(core)) {
            bool ok = false;
            std::tie(core, ok) = instrument_decreases(core, *decreases);
            dec_extra["decreases"] = *decreases;
            dec_extra["decreases_encoded"] = ok ? "true" : "false";
            if (!ok)
                return fail("decreases (" + *decreases + ") not instrumented",
                            {{"decreases", *decreases}, {"decreases_unencoded", "true"}});
        }
    }
    // A bare `return;` in a value-returning function returns an indeterminate
    // value, so an ensures about the result cannot hold on that path: never a
    // proof.
    if (ensures && !returns_void(fn) && has_empty_return(core))
        return fail("return without a value in a function returning a value; the ensures (" + *ensures +
                        ") cannot be checked on that path; not a proof",
                    {{"empty_return", "true"}});
    AcslC acsl;
    std::optional<std::string> req_c, ens_c;
    if (requires_) req_c = acsl_requires_c(*requires_, acsl);
    if (acsl.error.empty() && ensures) ens_c = acsl_ensures_c(*ensures, fn, acsl);
    if (!acsl.error.empty())
        return fail("cannot encode the contract for the checker: " + acsl.error + "; not a proof",
                    {{"acsl_unencoded", acsl.error}});
    auto cloned = fn;
    cloned.body = instrument_body(core, req_c, ens_c, fn, acsl);
    auto r = pir_one(cloned, unwind);
    r.stage = "contracts";
    if (r.cls.empty()) r.cls = "FUNC-CONTRACT";
    r.extra["requires"] = requires_ ? *requires_ : "";
    r.extra["ensures"] = ensures ? *ensures : "";
    r.extra["assumed"] = (requires_ || invariant) ? "true" : "false";
    r.extra["original_status"] = r.status;
    for (auto& [k, v] : inv_extra) r.extra[k] = v;
    for (auto& [k, v] : dec_extra) r.extra[k] = v;
    if (laws::is_proof(r.status) && (requires_ || invariant)) {
        r.status = std::string(laws::PROVED_ASSUMING);
        std::vector<std::string> parts;
        if (requires_) parts.push_back("(" + *requires_ + ")");
        if (invariant) parts.push_back("invariant (" + *invariant + ")");
        r.message = "ensures holds assuming " + join_sv(parts, " and ") + "; never an unconditional PROVED";
    }
    return r;
}

}  // namespace stages_detail

Finding prove_with_contract(const FunctionInfo& fn, int unwind, const std::optional<std::string>& requires_,
                            const std::optional<std::string>& ensures) {
    // Roadmap 4.2 / 9.2 (src/prism/ai/contracts.cpp, proof_repair.cpp): the
    // contracts engine for a contract that is not written in fn's comments.
    if (fn.kind != "SCALAR" || body_needs_pointer_harness(fn.body)) {
        auto f = make_find("contracts", laws::NEEDS_HARNESS, fn, "FUNC-CONTRACT",
                           fn.kind + ": contract scalar subset only; not a proof", laws::STRENGTH_READS);
        if (requires_) f.extra["requires"] = *requires_;
        if (ensures) f.extra["ensures"] = *ensures;
        return f;
    }
    return check_with_assume(fn, unwind, requires_, ensures, std::nullopt, std::nullopt);
}

namespace {

// The `assigns` clauses of the ACSL block that ends right above fn.
// parse_comments drops them: the checker asserts the ensures, not the frame,
// so a row whose contract has one says so (Law 7: nothing unchecked quietly).
std::vector<std::string> acsl_assigns(const FunctionInfo& fn) {
    std::vector<std::string> out;
    auto text = read_fn_source(fn);
    if (text.empty() || fn.line < 2) return out;
    std::vector<std::string> lines;
    {
        std::istringstream ss(text);
        std::string ln;
        while (std::getline(ss, ln)) lines.push_back(ln);
    }
    const int hi = std::min(static_cast<int>(lines.size()), fn.line - 1);  // lines before the function's
    std::string before;
    for (int i = std::max(0, hi - 80); i < hi; ++i) before += lines.at(static_cast<std::size_t>(i)) + "\n";
    auto end = before.rfind("*/");
    if (end == std::string::npos || !strip(before.substr(end + 2)).empty()) return out;
    auto open = before.rfind("/*", end);
    if (open == std::string::npos || before.compare(open, 3, "/*@") != 0) return out;
    std::istringstream cs(before.substr(open + 3, end - open - 3));
    std::string part;
    while (std::getline(cs, part, ';')) {
        auto c = strip(part);
        if (c.starts_with("assigns") && c.size() > 7 && std::isspace(static_cast<unsigned char>(c[7])))
            out.push_back(std::string(strip(c.substr(7))));
    }
    return out;
}

// The contracts row of one function (nullopt: no contract comment).
std::optional<Finding> contracts_one(const FunctionInfo& fn, int unwind) {
    static Regex req_atom("^([A-Za-z_]\\w*)\\s*(<|>=|!=)\\s*(0|[1-9]\\d*)$");
    static Regex ens_atom("^result\\s*(==|>=)\\s*(.+)$");
    auto spec = parse_comments(fn);
    if (spec.requires_.empty() && spec.ensures.empty() && spec.decreases.empty() && spec.invariant.empty())
        return std::nullopt;
    std::optional<std::string> requires_, ensures, decreases, invariant;
    // Clauses are conjoined; one that binds looser than && (A || B, A ==> B,
    // A ? B : C) keeps its parentheses, so `requires A || B; requires C;`
    // is (A || B) && C, never A || (B && C).
    auto conj = [](const std::vector<std::string>& v) {
        std::vector<std::string> parts;
        for (auto& c : v) {
            bool loose = c.find("||") != std::string::npos || c.find("==>") != std::string::npos ||
                         c.find('?') != std::string::npos || c.find(',') != std::string::npos;
            parts.push_back(loose && v.size() > 1 ? "(" + c + ")" : c);
        }
        return join_sv(parts, " && ");
    };
    if (!spec.requires_.empty()) requires_ = conj(spec.requires_);
    if (!spec.ensures.empty()) ensures = conj(spec.ensures);
    if (!spec.decreases.empty()) decreases = spec.decreases[0];
    if (!spec.invariant.empty()) invariant = join_sv(spec.invariant, " && ");
    bool ptr_body = body_needs_pointer_harness(fn.body);
    // A POINTER function whose every pointer parameter has a \valid /
    // \valid_read requires is checked like a scalar one: the pir stage binds
    // the stated objects (PROVED-ASSUMING with them listed, Law 6).
    const bool bound = fn.kind == "POINTER" && pointer_params_bound(fn, spec.requires_);
    if (!bound && (fn.kind == "POINTER" || ptr_body || fn.kind != "SCALAR")) {
        std::string why = (fn.kind == "POINTER" || ptr_body) ? "POINTER" : fn.kind;
        std::string msg = why == "POINTER"
                              ? "POINTER: the contract check would invent a buffer; not a proof"
                              : why + ": contract scalar subset only; not a proof";
        auto f = make_find("contracts", laws::NEEDS_HARNESS, fn, "FUNC-CONTRACT", std::move(msg),
                           laws::STRENGTH_PROVES);
        if (requires_) f.extra["requires"] = *requires_;
        if (ensures) f.extra["ensures"] = *ensures;
        if (decreases) f.extra["decreases"] = *decreases;
        if (invariant) f.extra["invariant"] = *invariant;
        return f;
    }
    auto rec = check_with_assume(fn, unwind, requires_, ensures, decreases, invariant);
    if (rec.status != laws::ERROR && invariant && has_loop(fn.body)) {
        auto baseline = check_with_assume(fn, unwind, requires_, ensures, decreases, std::nullopt);
        bool closed_without = baseline.status == laws::PROVED_UNBOUNDED;
        auto orig = rec.extra.contains("original_status") ? rec.extra["original_status"] : rec.status;
        if (laws::is_proof(orig)) {
            if (orig == laws::PROVED_UNBOUNDED) rec.extra["original_status"] = std::string(laws::PROVED_ASSUMING);
            if (laws::is_proof(rec.status) && rec.status == laws::PROVED_UNBOUNDED) {
                rec.status = std::string(laws::PROVED_ASSUMING);
                rec.message = "ensures holds assuming invariant (" + *invariant + "); never PROVED-UNBOUNDED";
            }
        } else if (closed_without && laws::is_proof(rec.status)) {
            rec.status = std::string(laws::PROVED_ASSUMING);
            rec.message = "ensures holds assuming invariant (" + *invariant + "); never PROVED-UNBOUNDED";
        }
    }
    if (rec.status != laws::ERROR && decreases && has_loop(fn.body)) {
        // Baseline = the same requires and ensures, minus only decreases.
        // The invariant stays. Python's bmc_function_with_assume(...,
        // decreases=None) leaves invariant at its default None, so that
        // baseline drops the invariant too. Copying that default is not
        // done: the main query still instruments decreases. When an
        // invariant is present, check_with_assume rewrites a proof to
        // PROVED-ASSUMING, so this baseline is not PROVED-UNBOUNDED.
        // decreases_assumed can only lower a status. It does not skip the
        // decreases check.
        auto baseline = check_with_assume(fn, unwind, requires_, ensures, std::nullopt, invariant);
        bool closed_without = baseline.status == laws::PROVED_UNBOUNDED;
        auto orig = rec.extra.contains("original_status") ? rec.extra["original_status"] : rec.status;
        bool assumed = laws::is_proof(orig) && !closed_without;
        rec.extra["decreases_assumed"] = assumed ? "true" : "false";
        if (assumed) {
            if (orig == laws::PROVED_UNBOUNDED) rec.extra["original_status"] = std::string(laws::PROVED_ASSUMING);
            if (laws::is_proof(rec.status) && rec.status == laws::PROVED_UNBOUNDED) {
                rec.status = std::string(laws::PROVED_ASSUMING);
                rec.message = "ensures holds under decreases variant; never PROVED-UNBOUNDED";
            }
        }
    }
    bool subset = true;
    for (auto& r : spec.requires_)
        if (!fullmatch(req_atom, strip(r))) subset = false;
    for (auto& e : spec.ensures)
        if (!fullmatch(ens_atom, strip(e))) subset = false;
    rec.extra["subset"] = subset ? "true" : "false";
    if (auto as = acsl_assigns(fn); !as.empty()) {
        rec.extra["assigns"] = join_sv(as, "; ");
        rec.extra["assigns_checked"] = "false";
        if (laws::is_proof(rec.status)) rec.message += " (assigns clause not checked)";
    }
    return rec;
}

}  // namespace

std::vector<Finding> prove_contracts(const std::vector<FunctionInfo>& functions, int unwind) {
    return prove_contracts(functions, unwind, 1);
}

// Every pir check of the stage is collected by a dry run and made in batched
// pir runs on `jobs` workers (prefetch_pir); the functions are then checked
// `jobs` at a time with those rows. The baselines (contract minus invariant
// or decreases) are asked by the dry run whatever the main check gives: one
// the real pass does not need was checked for nothing, never used.
std::vector<Finding> prove_contracts(const std::vector<FunctionInfo>& functions, int unwind, int jobs) {
    prefetch_pir(jobs, [&] {
        for (auto& fn : functions) (void)contracts_one(fn, unwind);
    });
    return map_functions(jobs, functions.size(), [&](std::size_t i) {
        std::vector<Finding> v;
        if (auto f = contracts_one(functions[i], unwind)) v.push_back(std::move(*f));
        return v;
    });
}

}  // namespace prism
