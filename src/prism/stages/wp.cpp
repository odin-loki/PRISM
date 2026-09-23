// Stage wp (the Python engine prism/wp.py): ensures proved by substituting the returned
// expression; unencodable ACSL is ERROR, never PROVED-ASSUMING.
#include "common.hpp"

namespace prism {
namespace fs = std::filesystem;
using namespace stages_detail;

namespace {
// Python engine prism/wp.py encode_predicate / _ACSL_UNENC. Regex is the honesty
// gate: unencodable ACSL is ERROR, never PROVED-ASSUMING.
static Regex wp_acsl_unenc(
    "(?i)\\\\(?:"
    "valid(?:_read|_write|_index)?|forall|exists|old|at|"
    "separated|initialized|dangling|null|base_addr|block_length|"
    "offset|allocable|freeable|fresh|from|nothing|let|lambda|"
    "true|false|union|inter|subset|empty|is_finite|is_NaN|"
    "min|max|sum|product|numof|matches|unspecified|allocation"
    ")\\b");

static Regex wp_tok(
    "\\s+|==>|==|!=|<=|>=|&&|\\|\\||<<|>>|->|"
    "[+\\-*/%<>=!&|^~?:()]|"
    "0[xX][0-9A-Fa-f]+|"
    "\\d+|"
    "[A-Za-z_]\\w*|"
    ".");

static Regex wp_ident_re("^[A-Za-z_]\\w*$");

static Regex wp_num_re("^(?:0[xX][0-9A-Fa-f]+|\\d+)$");

static Regex wp_result_re("\\bresult\\b");

const std::unordered_set<std::string> kWpBinop = {
    "+", "-", "*", "/", "%", "<", ">", "<=", ">=", "==", "!=",
    "&&", "||", "<<", ">>", "&", "|", "^", "?", ":",
};

const std::unordered_set<std::string> kWpUnaryOk = {"+", "-", "!", "~"};

bool wp_outer_parens(std::string_view s) {
    if (s.size() < 2 || s.front() != '(' || s.back() != ')') return false;
    int depth = 0;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '(') ++depth;
        else if (s[i] == ')') {
            --depth;
            if (depth == 0) return i + 1 == s.size();
        }
    }
    return false;
}

std::optional<std::pair<std::string, std::string>> wp_split_top(std::string_view text, std::string_view sep) {
    int depth = 0;
    const std::size_t n = sep.size();
    if (n == 0 || text.size() < n) return std::nullopt;
    for (std::size_t i = 0; i + n <= text.size(); ++i) {
        char ch = text[i];
        if (ch == '(') ++depth;
        else if (ch == ')') {
            --depth;
            if (depth < 0) return std::nullopt;
        } else if (depth == 0 && text.substr(i, n) == sep)
            return std::make_pair(std::string(text.substr(0, i)), std::string(text.substr(i + n)));
    }
    return std::nullopt;
}

std::optional<std::string> wp_rewrite_implies(std::string text) {
    text = strip(std::move(text));
    if (wp_outer_parens(text)) {
        auto inner = wp_rewrite_implies(text.substr(1, text.size() - 2));
        if (!inner) return std::nullopt;
        return "(" + *inner + ")";
    }
    auto split = wp_split_top(text, "==>");
    if (!split) return text;
    auto left = wp_rewrite_implies(split->first);
    auto right = wp_rewrite_implies(split->second);
    if (!left || !right) return std::nullopt;
    return "!(" + *left + ") || (" + *right + ")";
}

bool wp_before_unary(const std::optional<std::string>& prev) {
    if (!prev) return true;
    if (*prev == "(" || *prev == "?" || *prev == ":") return true;
    if (kWpBinop.contains(*prev)) return true;
    return false;
}

std::optional<std::string> wp_encode_scalar(const std::string& text) {
    std::vector<std::string> toks;
    for (auto& m : wp_tok.finditer(text)) {
        auto t = m.text;
        if (t.empty()) continue;
        bool sp = true;
        for (char c : t)
            if (!std::isspace(static_cast<unsigned char>(c))) {
                sp = false;
                break;
            }
        if (sp) continue;
        toks.push_back(std::move(t));
    }
    if (toks.empty()) return std::nullopt;
    std::optional<std::string> prev;
    for (std::size_t i = 0; i < toks.size(); ++i) {
        const auto& t = toks[i];
        if (t == "[" || t == "]" || t == "{" || t == "}" || t == "." || t == "," || t == ";" ||
            t == "@" || t == "\\" || t == "#")
            return std::nullopt;
        if (t == "->") return std::nullopt;
        if (fullmatch(wp_ident_re, t)) {
            const std::string* nxt = (i + 1 < toks.size()) ? &toks[i + 1] : nullptr;
            if (nxt && *nxt == "(") return std::nullopt;
            prev = t;
            continue;
        }
        if (fullmatch(wp_num_re, t)) {
            prev = t;
            continue;
        }
        if (kWpUnaryOk.contains(t) || t == "*" || t == "&") {
            bool unary = wp_before_unary(prev) || (prev && (kWpUnaryOk.contains(*prev) || *prev == "*" || *prev == "&"));
            if ((t == "*" || t == "&") && unary) return std::nullopt;
            if (kWpUnaryOk.contains(t) && unary) {
                prev = t;
                continue;
            }
        }
        if (kWpBinop.contains(t) || t == "(" || t == ")") {
            prev = t;
            continue;
        }
        return std::nullopt;
    }
    return strip(text);
}

std::optional<std::string> wp_encode_predicate(std::string raw) {
    raw = strip(std::move(raw));
    if (raw.empty()) return std::nullopt;
    {
        const std::string from = "\\result";
        std::size_t pos = 0;
        while ((pos = raw.find(from, pos)) != std::string::npos) {
            raw.replace(pos, from.size(), "result");
            pos += 6;
        }
    }
    if (wp_acsl_unenc.search(raw) || raw.find("\\ ") != std::string::npos || raw.starts_with("\\"))
        return std::nullopt;
    if (raw.find("<==>") != std::string::npos || raw.find("^^") != std::string::npos) return std::nullopt;
    auto rewritten = wp_rewrite_implies(raw);
    if (!rewritten) return std::nullopt;
    return wp_encode_scalar(*rewritten);
}

std::string wp_subst_result(const std::string& ensures, const std::string& expr) {
    std::string out;
    std::size_t i = 0;
    const std::string repl = "(" + expr + ")";
    for (auto& m : wp_result_re.finditer(ensures)) {
        if (m.spans.empty()) continue;
        auto a = static_cast<std::size_t>(std::max(0, m.spans[0].first));
        auto b = static_cast<std::size_t>(std::max(0, m.spans[0].second));
        if (a < i) continue;
        out.append(ensures, i, a - i);
        out += repl;
        i = b;
    }
    out.append(ensures, i, std::string::npos);
    return out;
}

std::string wp_norm_expr(std::string s) {
    s = strip(std::move(s));
    while (wp_outer_parens(s)) s = strip(s.substr(1, s.size() - 2));
    std::string o;
    for (char c : s)
        if (!std::isspace(static_cast<unsigned char>(c))) o.push_back(c);
    return o;
}

std::vector<std::string> wp_split_and(const std::string& text) {
    std::vector<std::string> out;
    int depth = 0;
    std::size_t last = 0;
    std::size_t i = 0;
    while (i + 1 < text.size()) {
        char ch = text[i];
        if (ch == '(') ++depth;
        else if (ch == ')') --depth;
        else if (depth == 0 && text[i] == '&' && text[i + 1] == '&') {
            auto p = strip(text.substr(last, i - last));
            if (!p.empty()) out.push_back(std::move(p));
            last = i + 2;
            i += 2;
            continue;
        }
        ++i;
    }
    auto tail = strip(text.substr(last));
    if (!tail.empty()) out.push_back(std::move(tail));
    return out;
}

bool wp_is_tautology(const std::string& pred) {
    auto p = strip(pred);
    if (p == "1" || p == "true" || p == "True") return true;
    auto parts = wp_split_and(p);
    if (parts.size() > 1) {
        for (auto& x : parts)
            if (!wp_is_tautology(x)) return false;
        return true;
    }
    for (const char* op : {"==", ">=", "<="}) {
        auto split = wp_split_top(p, op);
        if (!split) continue;
        auto a = wp_norm_expr(split->first);
        auto b = wp_norm_expr(split->second);
        if (!a.empty() && a == b) return true;
    }
    return false;
}

}  // namespace

std::vector<Finding> run_wp(const std::vector<FunctionInfo>& functions, int unwind) {
    std::vector<Finding> out;
    static Regex ret_re("\\breturn\\s+([^;]+);");
    for (auto& fn : functions) {
        auto spec = parse_comments(fn);
        if (spec.ensures.empty()) continue;
        auto mark = [](Finding& f) {
            f.extra["engine"] = "prism-wp";
            f.extra["wp"] = "return-substitution";
        };
        if (fn.kind == "POINTER" || body_needs_pointer_harness(fn.body)) {
            auto f = make_find("wp", laws::NEEDS_HARNESS, fn, "FUNC-CONTRACT",
                               "POINTER: WP would invent a buffer; Frama-C WP binary is not a proof either",
                               laws::STRENGTH_PROVES);
            mark(f);
            out.push_back(std::move(f));
            continue;
        }
        if (fn.kind != "SCALAR") {
            auto f = make_find("wp", laws::NEEDS_HARNESS, fn, "FUNC-CONTRACT",
                               fn.kind + ": WP scalar subset only; not a proof", laws::STRENGTH_PROVES);
            mark(f);
            out.push_back(std::move(f));
            continue;
        }

        std::vector<std::string> encoded_req;
        std::optional<std::string> bad;
        for (auto& r : spec.requires_) {
            auto e = wp_encode_predicate(r);
            if (!e) {
                bad = r;
                break;
            }
            encoded_req.push_back(*e);
        }
        std::vector<std::string> encoded_ens;
        if (!bad) {
            for (auto& e0 : spec.ensures) {
                auto e = wp_encode_predicate(e0);
                if (!e) {
                    bad = e0;
                    break;
                }
                encoded_ens.push_back(*e);
            }
        }
        if (bad) {
            auto f = make_find("wp", laws::ERROR, fn, "FUNC-CONTRACT",
                               "cannot encode WP predicate (" + *bad + ")", laws::STRENGTH_PROVES);
            mark(f);
            f.extra["wp_unencoded"] = "true";
            if (!spec.requires_.empty()) f.extra["requires"] = join_sv(spec.requires_, " && ");
            f.extra["ensures"] = join_sv(spec.ensures, " && ");
            out.push_back(std::move(f));
            continue;
        }

        std::optional<std::string> requires_;
        if (!encoded_req.empty()) requires_ = join_sv(encoded_req, " && ");
        std::string ensures = join_sv(encoded_ens, " && ");
        std::vector<std::string> returns;
        nlohmann::json rets = nlohmann::json::array();
        for (auto& m : ret_re.finditer(fn.body)) {
            auto g = strip(m.group(1));
            if (g.empty()) continue;
            returns.push_back(g);
            rets.push_back(g);
            if (returns.size() >= 8) break;
        }
        std::vector<std::string> vcs;
        if (returns.empty()) vcs.push_back(ensures);
        else
            for (auto& expr : returns) vcs.push_back(wp_subst_result(ensures, expr));
        std::string wp_vc;
        if (requires_) wp_vc += "(" + *requires_ + ") ==> ";
        for (std::size_t i = 0; i < vcs.size(); ++i) {
            if (i) wp_vc += " && ";
            wp_vc += "(" + vcs[i] + ")";
        }
        auto fill_extra = [&](Finding& rec) {
            mark(rec);
            rec.extra["wp_returns"] = rets.dump();
            rec.extra["wp_vc"] = wp_vc;
            rec.extra["requires"] = requires_ ? *requires_ : "";
            rec.extra["ensures"] = ensures;
        };

        bool qed = !vcs.empty();
        for (auto& v : vcs)
            if (!wp_is_tautology(v)) qed = false;
        if (qed) {
            auto f = make_find("wp", laws::PROVED_ASSUMING, fn, "FUNC-CONTRACT",
                               "WP of ensures (" + ensures + ") holds" +
                                   (requires_ ? " assuming (" + *requires_ + ")" : "") +
                                   "; never an unconditional PROVED",
                               laws::STRENGTH_PROVES);
            fill_extra(f);
            f.extra["wp_qed"] = "true";
            out.push_back(std::move(f));
            continue;
        }

        std::optional<std::string> ens_opt = ensures;
        auto rec = bmc_with_assume(fn, unwind, requires_, ens_opt, std::nullopt, std::nullopt);
        rec.stage = "wp";
        fill_extra(rec);
        if (rec.status == laws::PROVED) {
            rec.status = std::string(laws::PROVED_ASSUMING);
            rec.message = "WP of ensures (" + ensures + ") holds" +
                          (requires_ ? " assuming (" + *requires_ + ")" : "") +
                          "; never an unconditional PROVED";
        } else if (rec.status == laws::PROVED_UNBOUNDED) {
            rec.status = std::string(laws::PROVED_ASSUMING);
            rec.message = "WP of ensures (" + ensures + ") holds for all unrollings" +
                          (requires_ ? " assuming (" + *requires_ + ")" : "") +
                          "; never PROVED-UNBOUNDED from WP";
        } else if (rec.status == laws::PROVED_ASSUMING) {
            auto low = lower_copy(rec.message);
            if (low.find("never") == std::string::npos) {
                rec.message = "WP of ensures (" + ensures + ") holds" +
                              (requires_ ? " assuming (" + *requires_ + ")" : "") +
                              "; never an unconditional PROVED";
            }
        }
        out.push_back(std::move(rec));
    }
    return out;
}

}  // namespace prism
