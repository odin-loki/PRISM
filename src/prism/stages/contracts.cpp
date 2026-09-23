// Stage contracts: requires/ensures/invariant/decreases comments proved by
// BMC (prove_contracts, prove_with_contract; bmc_with_assume is shared with wp).
#include "common.hpp"

namespace prism {
namespace fs = std::filesystem;
using namespace stages_detail;

namespace {
std::pair<std::string, std::string> take_block_keep_braces(std::string text) {
    text = lstrip(std::move(text));
    if (text.starts_with("{")) {
        auto [inner, rest] = brace(text);
        return {"{" + inner + "}", rest};
    }
    return stmt(text);
}

bool has_loop(const std::string& body) { return re_search("\\b(while|for)\\b", body); }

bool encode_decreases(const std::string& expr) {
    // Simple identifier only. Compound measures (n - i, n - 1, *, tuples)
    // are ERROR, never PROVED-ASSUMING: Python engine does not decide their
    // well-foundedness the way Dafny's VC generator does.
    static Regex re("^[A-Za-z_]\\w*$");
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
        if (i + 1 < expr.size() && expr[i] == '&' && expr[i + 1] == '&') {
            auto p = strip(cur);
            if (!p.empty()) ps.push_back(p);
            cur.clear();
            i += 2;
        } else {
            cur.push_back(expr[i++]);
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

std::string rewrite_returns(const std::string& body, const std::string& ensures) {
    static Regex re("\\breturn\\s+([^;]+);");
    std::string out;
    std::size_t i = 0;
    for (auto& m : re.finditer(body)) {
        if (m.spans.empty()) continue;
        auto a = static_cast<std::size_t>(std::max(0, m.spans[0].first));
        auto b = static_cast<std::size_t>(std::max(0, m.spans[0].second));
        if (a < i) continue;
        out.append(body, i, a - i);
        auto expr = strip(m.group(1));
        if (expr.empty()) out.append(body, a, b - a);
        else
            out += "{ int result = " + expr + "; assert(" + ensures + "); return result; }";
        i = b;
    }
    out.append(body, i, std::string::npos);
    return out;
}

std::string instrument_body(const std::string& body, const std::optional<std::string>& requires_,
                            const std::optional<std::string>& ensures) {
    auto core = body;
    if (ensures) core = rewrite_returns(body, *ensures);
    std::string parts;
    if (requires_) parts += "if (!(" + *requires_ + ")) return 0;\n";
    parts += core;
    if (ensures && !re_search("\\breturn\\b", body)) parts += "\nassert(" + *ensures + ");";
    return parts;
}

}  // namespace

namespace stages_detail {
Finding bmc_with_assume(const FunctionInfo& fn, int unwind, const std::optional<std::string>& requires_,
                        const std::optional<std::string>& ensures, const std::optional<std::string>& decreases,
                        const std::optional<std::string>& invariant) {
    auto core = fn.body;
    std::map<std::string, std::string> inv_extra, dec_extra;
    auto fail = [&](std::string msg, std::map<std::string, std::string> extra) {
        auto f = make_find("contracts", laws::ERROR, fn, "FUNC-CONTRACT", std::move(msg), laws::STRENGTH_PROVES);
        f.extra = std::move(extra);
        if (requires_) f.extra["requires"] = *requires_;
        if (ensures) f.extra["ensures"] = *ensures;
        return f;
    };
    if (invariant && has_loop(core)) {
        if (!encode_invariant(*invariant))
            return fail("cannot encode invariant (" + *invariant + ") for BMC",
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
            return fail("cannot encode decreases (" + *decreases + ") for BMC",
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
    auto cloned = fn;
    cloned.body = instrument_body(core, requires_, ensures);
    auto r = bmc_one(cloned, unwind);
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
                           fn.kind + ": contract scalar subset only; not a proof", laws::STRENGTH_PROVES);
        if (requires_) f.extra["requires"] = *requires_;
        if (ensures) f.extra["ensures"] = *ensures;
        return f;
    }
    return bmc_with_assume(fn, unwind, requires_, ensures, std::nullopt, std::nullopt);
}

std::vector<Finding> prove_contracts(const std::vector<FunctionInfo>& functions, int unwind) {
    std::vector<Finding> out;
    static Regex req_atom("^([A-Za-z_]\\w*)\\s*(<|>=|!=)\\s*(0|[1-9]\\d*)$");
    static Regex ens_atom("^result\\s*(==|>=)\\s*(.+)$");
    for (auto& fn : functions) {
        auto spec = parse_comments(fn);
        if (spec.requires_.empty() && spec.ensures.empty() && spec.decreases.empty() && spec.invariant.empty())
            continue;
        std::optional<std::string> requires_, ensures, decreases, invariant;
        if (!spec.requires_.empty()) requires_ = join_sv(spec.requires_, " && ");
        if (!spec.ensures.empty()) ensures = join_sv(spec.ensures, " && ");
        if (!spec.decreases.empty()) decreases = spec.decreases[0];
        if (!spec.invariant.empty()) invariant = join_sv(spec.invariant, " && ");
        bool ptr_body = body_needs_pointer_harness(fn.body);
        if (fn.kind == "POINTER" || ptr_body || fn.kind != "SCALAR") {
            std::string why = (fn.kind == "POINTER" || ptr_body) ? "POINTER" : fn.kind;
            std::string msg = why == "POINTER"
                                  ? "POINTER: contract BMC would invent a buffer; not a proof"
                                  : why + ": contract scalar subset only; not a proof";
            auto f = make_find("contracts", laws::NEEDS_HARNESS, fn, "FUNC-CONTRACT", std::move(msg),
                               laws::STRENGTH_PROVES);
            if (requires_) f.extra["requires"] = *requires_;
            if (ensures) f.extra["ensures"] = *ensures;
            if (decreases) f.extra["decreases"] = *decreases;
            if (invariant) f.extra["invariant"] = *invariant;
            out.push_back(std::move(f));
            continue;
        }
        auto rec = bmc_with_assume(fn, unwind, requires_, ensures, decreases, invariant);
        if (rec.status != laws::ERROR && invariant && has_loop(fn.body)) {
            auto baseline = bmc_with_assume(fn, unwind, requires_, ensures, decreases, std::nullopt);
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
            auto baseline = bmc_with_assume(fn, unwind, requires_, ensures, std::nullopt, invariant);
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
        out.push_back(std::move(rec));
    }
    return out;
}

}  // namespace prism
