// Harness drafting for POINTER-parameter functions (roadmap 4.2, 9.2).
//
// Law 6 keeps pointer functions out of unguarded BMC. A drafted harness makes
// the missing preconditions explicit: the pointer is not NULL, it addresses
// exactly `n` elements (n taken from how the body indexes it), and the size
// lies in a small checked range. Every assumption is listed in the finding;
// the best verdict is PROVED-ASSUMING, never PROVED. A counterexample found
// under a drafted (not user-written) assumption is not reported as a defect:
// the assumption may be too narrow, so the row stays NEEDS-HARNESS with the
// counterexample attached for a reviewer.

#include "ai_internal.hpp"

#include "prism/laws.hpp"
#include "prism/stages.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <map>
#include <regex>
#include <set>

namespace prism::ai {
namespace {

constexpr int kMaxCase = 4;  // sizes 1..4 are checked (loops close under the default unwind)

bool int_element(std::string typ) {
    for (auto* q : {"const", "volatile", "restrict", "__restrict", "*", "[", "]"})
        for (std::size_t p; (p = typ.find(q)) != std::string::npos;) typ.erase(p, std::strlen(q));
    typ = trim(typ);
    while (typ.find("  ") != std::string::npos) typ.erase(typ.find("  "), 1);
    return typ == "int" || typ == "signed" || typ == "signed int" || typ == "int32_t";
}

bool looks_like_length(const std::string& n) {
    static const std::set<std::string> names{"n", "len", "length", "size", "count", "num", "cnt", "nelem",
                                             "nmemb", "nitems", "sz", "m", "k"};
    if (names.count(n)) return true;
    for (auto* suf : {"_len", "_size", "_count", "_n", "len", "size", "count"})
        if (n.size() > std::strlen(suf) && n.ends_with(suf)) return true;
    return false;
}

struct PtrUse {
    bool ok = true;
    std::string why;
    std::set<long long> literal_idx;
    bool symbolic_idx = false;
};

PtrUse pointer_uses(const std::string& body, const std::string& p) {
    PtrUse u;
    std::regex word("\\b" + p + "\\b");
    for (auto it = std::sregex_iterator(body.begin(), body.end(), word); it != std::sregex_iterator(); ++it) {
        auto pos = static_cast<std::size_t>(it->position());
        auto after = pos + p.size();
        while (after < body.size() && body[after] == ' ') ++after;
        std::size_t before = pos;
        while (before > 0 && body[before - 1] == ' ') --before;
        char prev = before > 0 ? body[before - 1] : ';';
        if (after < body.size() && body[after] == '[') {
            auto close = body.find(']', after);
            if (close == std::string::npos) return (u.ok = false, u.why = "unbalanced [", u);
            auto idx = trim(body.substr(after + 1, close - after - 1));
            if (!idx.empty() && std::all_of(idx.begin(), idx.end(), ::isdigit)) u.literal_idx.insert(std::stoll(idx));
            else u.symbolic_idx = true;
            continue;
        }
        if (prev == '*' ) {
            // *p (deref) — but not a multiplication a * p: require a statement
            // or expression start before the '*'.
            std::size_t b2 = before - 1;
            while (b2 > 0 && body[b2 - 1] == ' ') --b2;
            char pp = b2 > 0 ? body[b2 - 1] : ';';
            bool after_return = b2 >= 6 && body.compare(b2 - 6, 6, "return") == 0 &&
                                (b2 == 6 || !(std::isalnum(static_cast<unsigned char>(body[b2 - 7])) || body[b2 - 7] == '_'));
            if (after_return || std::string(";{}(=,!<>&|+-?:").find(pp) != std::string::npos) {
                u.literal_idx.insert(0);
                continue;
            }
        }
        return (u.ok = false, u.why = "pointer '" + p + "' used other than p[i] / *p (arithmetic, aliasing or call)", u);
    }
    return u;
}

// p == NULL / p == 0 / !p  -> 0 ;  p != NULL / p != 0 -> 1  (exactly the nonnull assumption)
std::string apply_nonnull(std::string body, const std::string& p) {
    body = std::regex_replace(body, std::regex("\\b" + p + "\\s*==\\s*(NULL|0|nullptr)\\b"), "0");
    body = std::regex_replace(body, std::regex("\\b(NULL|0|nullptr)\\s*==\\s*" + p + "\\b"), "0");
    body = std::regex_replace(body, std::regex("\\b" + p + "\\s*!=\\s*(NULL|0|nullptr)\\b"), "1");
    body = std::regex_replace(body, std::regex("\\b(NULL|0|nullptr)\\s*!=\\s*" + p + "\\b"), "1");
    body = std::regex_replace(body, std::regex("!\\s*" + p + "\\b(?!\\s*\\[)"), "0");
    body = std::regex_replace(body, std::regex("\\(\\s*" + p + "\\s*\\)"), "(1)");
    return body;
}

// *p -> p[0] (the array model has no pointer dereference of its own).
std::string deref_to_index(std::string body, const std::string& p) {
    body = std::regex_replace(body, std::regex("(^|[^\\w\\)\\]\\s])(\\s*)\\*\\s*" + p + "\\b"), "$1$2" + p + "[0]");
    body = std::regex_replace(body, std::regex("\\breturn\\s*\\*\\s*" + p + "\\b"), "return " + p + "[0]");
    return body;
}

struct Plan {
    std::vector<std::string> ptrs;
    std::map<std::string, std::string> size_of;  // ptr -> scalar param name or literal
    std::map<std::string, std::pair<int, int>> range;  // scalar -> checked [lo, hi]
    std::vector<std::string> assumptions;
    std::string source = "template";
};

std::vector<HarnessDraft> build(const FunctionInfo& fn, const Plan& plan, std::string* why) {
    std::vector<std::pair<std::string, std::string>> scalars;
    for (auto& [t, n] : fn.params)
        if (!n.empty() && std::find(plan.ptrs.begin(), plan.ptrs.end(), n) == plan.ptrs.end()) scalars.push_back({t, n});
    std::string body = fn.body;
    for (auto& p : plan.ptrs) body = deref_to_index(apply_nonnull(body, p), p);
    // Enumerate the size cases: one symbolic length variable at most.
    std::string lenvar;
    for (auto& [p, s] : plan.size_of)
        if (is_identifier(s)) {
            if (!lenvar.empty() && lenvar != s) {
                if (why) *why = "two different length parameters";
                return {};
            }
            lenvar = s;
        }
    std::vector<int> cases{0};
    if (!lenvar.empty()) {
        cases.clear();
        auto [lo, hi] = plan.range.count(lenvar) ? plan.range.at(lenvar) : std::pair<int, int>{1, kMaxCase};
        for (int k = std::max(1, lo); k <= std::min(hi, std::max(1, lo) + kMaxCase - 1); ++k) cases.push_back(k);
        if (cases.empty()) {
            if (why) *why = "empty size range";
            return {};
        }
    }
    std::vector<HarnessDraft> out;
    for (int k : cases) {
        std::string pre;
        for (auto& p : plan.ptrs) {
            auto s = plan.size_of.at(p);
            int n = is_identifier(s) ? k : std::stoi(s);
            pre += "int " + p + "[" + std::to_string(n) + "];\n";
        }
        if (!lenvar.empty()) pre += "if (!(" + lenvar + " == " + std::to_string(k) + ")) return 0;\n";
        for (auto& [v, r] : plan.range)
            if (v != lenvar)
                pre += "if (!(" + v + " >= " + std::to_string(r.first) + " && " + v + " <= " +
                       std::to_string(r.second) + ")) return 0;\n";
        HarnessDraft d;
        d.harnessed = fn;
        d.harnessed.params = scalars;
        d.harnessed.kind = scalars.empty() ? "VOID" : "SCALAR";
        d.harnessed.body = pre + body;
        d.assumptions = plan.assumptions;
        d.source = plan.source;
        out.push_back(std::move(d));
    }
    return out;
}

std::optional<Plan> template_plan(const FunctionInfo& fn, std::string* why) {
    auto say = [&](std::string m) -> std::optional<Plan> {
        if (why) *why = std::move(m);
        return std::nullopt;
    };
    if (fn.kind != "POINTER") return say("not a POINTER function");
    if (fn.body.find("->") != std::string::npos || std::regex_search(fn.body, std::regex("\\w\\s*\\.\\s*[A-Za-z_]")))
        return say("struct member access (no struct model)");
    Plan plan;
    std::vector<std::string> scal;
    for (auto& [t, n] : fn.params) {
        if (n.empty()) return say("unnamed parameter");
        bool ptr = t.find('*') != std::string::npos || t.find('[') != std::string::npos;
        if (!ptr) {
            scal.push_back(n);
            continue;
        }
        if (std::count(t.begin(), t.end(), '*') + std::count(t.begin(), t.end(), '[') > 1 || t.find('(') != std::string::npos)
            return say("multi-level or function pointer '" + t + "'");
        if (!int_element(t)) return say("element type of '" + n + "' is not int (32-bit array model only)");
        plan.ptrs.push_back(n);
    }
    if (plan.ptrs.empty()) return say("no pointer parameter");
    std::string nb = fn.body;
    for (auto& p : plan.ptrs) nb = apply_nonnull(nb, p);
    for (auto& p : plan.ptrs) {
        auto u = pointer_uses(nb, p);
        if (!u.ok) return say(u.why);
        std::string size;
        if (u.symbolic_idx) {
            // (a) a loop bound that guards the index: i < n with n a scalar parameter
            std::smatch m;
            std::string b = fn.body;
            std::regex bound("\\b([A-Za-z_]\\w*)\\s*<\\s*([A-Za-z_]\\w*)\\b");
            for (auto it = std::sregex_iterator(b.begin(), b.end(), bound); it != std::sregex_iterator(); ++it) {
                auto rhs = (*it)[2].str();
                if (std::find(scal.begin(), scal.end(), rhs) != scal.end()) {
                    size = rhs;
                    break;
                }
            }
            // (b) a conventional length name among the scalar parameters
            if (size.empty())
                for (auto& s : scal)
                    if (looks_like_length(s)) {
                        size = s;
                        break;
                    }
            if (size.empty()) return say("no length parameter for symbolic index into '" + p + "'");
            plan.assumptions.push_back(p + " points to exactly " + size + " int elements (length from usage)");
        } else {
            long long mx = u.literal_idx.empty() ? 0 : *u.literal_idx.rbegin();
            if (mx >= 4096) return say("literal index too large");
            size = std::to_string(mx + 1);
            plan.assumptions.push_back(p + " points to at least " + size + " int element(s)");
        }
        plan.size_of[p] = size;
        plan.assumptions.insert(plan.assumptions.end() - 1, p + " != NULL");
    }
    for (auto& [p, s] : plan.size_of)
        if (is_identifier(s) && !plan.range.count(s)) {
            plan.range[s] = {1, kMaxCase};
            plan.assumptions.push_back("1 <= " + s + " <= " + std::to_string(kMaxCase) +
                                       " (drafted size range; larger " + s + " unchecked)");
        }
    plan.assumptions.push_back("element values unconstrained");
    return plan;
}

std::optional<Plan> llm_plan(const FunctionInfo& fn, ModelBackend& backend, AuditRecord& ar, std::string* why) {
    std::vector<std::string> names;
    for (auto& [t, n] : fn.params)
        if (!n.empty()) names.push_back(n);
    ModelRequest req;
    req.feature = "harness";
    req.grammar = "harness";
    req.grammar_text = grammar_for("harness", names);
    req.system = system_prompt(
        "Task: draft the preconditions a caller must meet for this C function: which pointer parameters "
        "must be non-NULL, how many int elements each pointer addresses (a parameter name or a number), and "
        "small integer ranges for parameters. Reply with the JSON object only.");
    req.user = "Parameters: " + join(names, ", ") + "\n" + fence_untrusted(function_source(fn), "SOURCE");
    ar.function = fn.name;
    ar.file = fn.file;
    auto reply = ask(backend, req, ar);
    if (!reply.error.empty()) {
        if (why) *why = reply.error;
        ar.checker = "none";
        return std::nullopt;
    }
    auto v = validate_harness(reply.text, names);
    if (!v.ok) {
        ar.rejected_reason = v.reason;
        ar.checker = "grammar-validator";
        ar.checker_result = "rejected";
        if (why) *why = "model output rejected: " + v.reason;
        return std::nullopt;
    }
    ar.output_valid = true;
    Plan plan;
    plan.source = "llm:" + backend.name();
    std::set<std::string> nonnull;
    for (auto& [kind, text] : v.pairs) {
        if (kind == "nonnull") nonnull.insert(text);
        if (kind == "size") {
            auto c = text.find(':');
            plan.size_of[text.substr(0, c)] = text.substr(c + 1);
        }
        if (kind == "range") {
            auto c1 = text.find(':'), c2 = text.rfind(':');
            plan.range[text.substr(0, c1)] = {std::stoi(text.substr(c1 + 1, c2 - c1 - 1)), std::stoi(text.substr(c2 + 1))};
        }
    }
    for (auto& [t, n] : fn.params) {
        bool ptr = t.find('*') != std::string::npos || t.find('[') != std::string::npos;
        if (!ptr) continue;
        if (!int_element(t) || !nonnull.count(n) || !plan.size_of.count(n)) {
            if (why) *why = "model draft does not cover pointer '" + n + "' (nonnull + size, int elements)";
            ar.checker = "harness-builder";
            ar.checker_result = "rejected";
            return std::nullopt;
        }
        auto u = pointer_uses(apply_nonnull(fn.body, n), n);
        if (!u.ok) {
            if (why) *why = u.why;
            ar.checker = "harness-builder";
            ar.checker_result = "rejected";
            return std::nullopt;
        }
        plan.ptrs.push_back(n);
        plan.assumptions.push_back(n + " != NULL");
        plan.assumptions.push_back(n + " points to " + plan.size_of[n] + " int element(s)");
    }
    for (auto& [s, r] : plan.range) {
        int hi = std::min(r.second, std::max(1, r.first) + kMaxCase - 1);
        plan.assumptions.push_back(std::to_string(r.first) + " <= " + s + " <= " + std::to_string(r.second) +
                                   (hi < r.second ? " (checked up to " + std::to_string(hi) + ")" : ""));
    }
    plan.assumptions.push_back("element values unconstrained");
    return plan;
}

}  // namespace

std::vector<HarnessDraft> draft_harness(const FunctionInfo& fn, std::string* why) {
    auto plan = template_plan(fn, why);
    if (!plan) return {};
    return build(fn, *plan, why);
}

std::optional<Finding> drafted_harness_bmc(const FunctionInfo& fn, int unwind) {
    if (fn.kind != "POINTER") return std::nullopt;
    std::string why;
    auto drafts = draft_harness(fn, &why);
    std::optional<AuditRecord> ar;
    std::string model_note;
    if (drafts.empty()) {
        std::string unavailable;
        auto backend = session_backend(&unavailable);
        if (!backend) {
            model_note = "NOTRUN: " + unavailable;
        } else {
            ar.emplace();
            std::string lwhy;
            auto plan = llm_plan(fn, *backend, *ar, &lwhy);
            if (plan) drafts = build(fn, *plan, &lwhy);
            if (drafts.empty()) {
                model_note = "model draft unusable: " + lwhy;
                if (ar->checker.empty()) ar->checker = "harness-builder";
                if (ar->checker_result.empty()) ar->checker_result = "rejected";
                audit_append(*ar);
            }
        }
    }
    if (drafts.empty()) return std::nullopt;
    auto& A = drafts.front().assumptions;
    nlohmann::json aj = A;
    Finding worst;
    std::vector<std::string> statuses;
    std::optional<Finding> failed;
    bool all_proof = true, any_bounded = false;
    std::string blocking;
    for (auto& d : drafts) {
        auto recs = run_bmc({d.harnessed}, unwind, true);
        auto r = recs.empty() ? Finding{} : recs[0];
        statuses.push_back(r.status);
        if (r.status == laws::FAILED) {
            if (!failed) failed = r;
            all_proof = false;
        } else if (r.status == laws::BOUNDED) {
            any_bounded = true;
            all_proof = false;
        } else if (r.status != laws::PROVED && r.status != laws::PROVED_UNBOUNDED) {
            all_proof = false;
            if (blocking.empty()) blocking = r.status + ": " + r.message;
        }
    }
    Finding f;
    f.stage = "harness";
    f.file = fn.file;
    f.function = fn.name;
    f.line = fn.line;
    f.strength = std::string(laws::STRENGTH_PROVES);
    f.extra["harness"] = "drafted";
    f.extra["harness_source"] = drafts.front().source;
    f.extra["assumed"] = "true";
    f.extra["assumptions"] = aj.dump();
    f.extra["draft_cases"] = nlohmann::json(statuses).dump();
    if (!model_note.empty()) f.extra["llm_harness"] = model_note;
    auto listed = join(A, "; ");
    if (all_proof) {
        f.status = std::string(laws::PROVED_ASSUMING);
        f.extra["original_status"] = statuses.front();
        f.message = "encoded UB properties hold assuming drafted harness: " + listed +
                    "; never an unconditional PROVED";
    } else if (failed) {
        f.status = std::string(laws::NEEDS_HARNESS);
        f.strength = std::string(laws::STRENGTH_SOME);
        f.cls = "";
        f.extra["draft_cls"] = failed->cls;
        f.extra["draft_cex"] = failed->counterexample;
        f.message = "drafted harness (" + listed + ") admits a counterexample (" + failed->cls + ": " +
                    failed->counterexample + "); a drafted assumption is not a caller contract, so this is "
                    "not reported as a defect: confirm the assumptions as `// requires:` to decide it";
    } else if (any_bounded && blocking.empty()) {
        f.status = std::string(laws::BOUNDED);
        f.message = "no violation within unwind " + std::to_string(unwind) + " assuming drafted harness: " + listed;
    } else {
        f.status = std::string(laws::NEEDS_HARNESS);
        f.strength = std::string(laws::STRENGTH_SOME);
        f.extra["harness"] = "false";
        f.message = "drafted harness (" + listed + ") not decidable by BMC: " + blocking;
    }
    if (ar) {
        ar->checker = "bmc(drafted harness)";
        ar->checker_result = f.status;
        ar->verdict_effect = laws::is_proof(f.status) ? f.status : "none";
        audit_append(*ar);
        f.extra["ai_audit_id"] = ar->id;
        f.extra["ai_checker"] = ar->checker;
        f.extra["ai_checker_result"] = ar->checker_result;
    }
    return f;
}

}  // namespace prism::ai
