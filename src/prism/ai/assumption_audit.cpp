// Assumption auditing (roadmap 9.3).
//
// Deterministic half (no model): a precondition that no input satisfies makes
// every PROVED-ASSUMING under it vacuous. The conjunction of a finding's
// `requires` clauses and its drafted integer ranges is checked with the
// bitvector encoder (C semantics, parameter widths); UNSAT is a
// VACUOUS-ASSUMPTION finding (FAILED: the specification or harness is
// wrong), SAT is recorded, anything the encoder cannot express is named as
// not audited.
//
// Model half: a second, independent model pass reviews each assumption and
// flags any that could exclude a realistic input. Flags are READS findings
// for the reviewer; they never change a verdict.

#include "ai_internal.hpp"

#include "prism/ai_proof.hpp"
#include "prism/laws.hpp"

#include <nlohmann/json.hpp>

#include <regex>
#include <set>

namespace prism::ai {

namespace {

bool integer_type(const std::string& typ) {
    if (typ.find('*') != std::string::npos || typ.find('[') != std::string::npos) return false;
    for (auto* bad : {"float", "double", "struct", "union", "bool", "_Bool", "void"})
        if (typ.find(bad) != std::string::npos) return false;
    return true;
}

std::vector<std::string> int_params(const FunctionInfo& fn) {
    std::vector<std::string> out;
    for (auto& [t, n] : fn.params)
        if (!n.empty() && integer_type(t)) out.push_back(n);
    return out;
}

Finding base_finding(const std::string& stage, std::string_view status, const FunctionInfo* fn,
                     const Finding* about, std::string cls, std::string msg, std::string_view strength) {
    Finding f;
    f.stage = stage;
    f.status = std::string(status);
    if (fn) {
        f.file = fn->file;
        f.function = fn->name;
        f.line = fn->line;
    } else if (about) {
        f.file = about->file;
        f.function = about->function;
        f.line = about->line;
    }
    f.cls = std::move(cls);
    f.message = std::move(msg);
    f.strength = std::string(strength);
    return f;
}

const FunctionInfo* find_fn(const std::vector<FunctionInfo>& fns, const Finding& f) {
    if (!f.function) return nullptr;
    for (auto& fn : fns)
        if (fn.name == *f.function && (f.file.empty() || fn.file == f.file)) return &fn;
    for (auto& fn : fns)
        if (fn.name == *f.function) return &fn;
    return nullptr;
}

}  // namespace

// Top-level && split (parentheses respected).
std::vector<std::string> split_conjuncts(const std::string& expr) {
    std::vector<std::string> out;
    int depth = 0;
    std::size_t start = 0;
    for (std::size_t i = 0; i < expr.size(); ++i) {
        char c = expr[i];
        if (c == '(') ++depth;
        else if (c == ')') --depth;
        else if (depth == 0 && c == '&' && i + 1 < expr.size() && expr[i + 1] == '&') {
            auto part = trim(expr.substr(start, i - start));
            if (!part.empty()) out.push_back(part);
            start = i + 2;
            ++i;
        }
    }
    auto part = trim(expr.substr(start));
    if (!part.empty()) out.push_back(part);
    // Strip one layer of redundant outer parentheses.
    for (auto& p : out) {
        while (p.size() >= 2 && p.front() == '(' && p.back() == ')') {
            int d = 0;
            bool outer = true;
            for (std::size_t i = 0; i < p.size(); ++i) {
                if (p[i] == '(') ++d;
                else if (p[i] == ')' && --d == 0 && i + 1 != p.size()) outer = false;
            }
            if (!outer) break;
            p = trim(p.substr(1, p.size() - 2));
        }
    }
    return out;
}

std::string assumptions_satisfiable(const FunctionInfo& fn, const std::vector<std::string>& clauses) {
    if (clauses.empty()) return "sat";
    auto vars = int_params(fn);
    std::string conj;
    for (auto& c : clauses) {
        std::string why;
        if (!valid_c_bool_expr(c, vars, &why)) return "unknown: '" + c + "' is not an integer expression over the parameters (" + why + ")";
        if (!conj.empty()) conj += " && ";
        conj += "(" + c + ")";
    }
    FunctionInfo probe = fn;
    probe.body = "__prism_assume(" + conj + "); __prism_assert(1, 0);";
    auto pc = check_program(probe, probe.body, 1, 4000, /*only_invariants=*/true);
    if (!pc.encoded) return "unknown: " + (pc.error.empty() ? std::string("not encoded") : pc.error);
    for (auto& p : pc.props) {
        if (p.name != "ai-inv#1") continue;
        if (p.result == "unsat") return "unsat";
        if (p.result == "sat") return "sat";
        return "unknown: solver " + p.result;
    }
    // No property recorded: make no claim either way.
    return "unknown: probe property missing";
}

namespace {

// (assumption text, C clause or "") for one finding: contract requires and
// drafted integer ranges ("1 <= n <= 4 (...)").
std::vector<std::pair<std::string, std::string>> finding_assumptions(const Finding& f) {
    std::vector<std::pair<std::string, std::string>> out;
    auto add_requires = [&](const std::string& req) {
        for (auto& c : split_conjuncts(req)) out.push_back({"requires " + c, c});
    };
    if (auto it = f.extra.find("requires"); it != f.extra.end() && !trim(it->second).empty()) {
        auto r = trim(it->second);
        if (r.front() == '[') {
            try {
                for (auto& x : nlohmann::json::parse(r))
                    if (x.is_string()) add_requires(x.get<std::string>());
            } catch (...) {
            }
        } else {
            add_requires(r);
        }
    }
    if (auto it = f.extra.find("assumptions"); it != f.extra.end()) {
        static const std::regex range(R"(^\s*(-?\d+)\s*<=\s*([A-Za-z_]\w*)\s*<=\s*(-?\d+))");
        try {
            for (auto& x : nlohmann::json::parse(it->second)) {
                if (!x.is_string()) continue;
                auto a = x.get<std::string>();
                std::smatch m;
                if (std::regex_search(a, m, range))
                    out.push_back({a, m[2].str() + " >= " + m[1].str() + " && " + m[2].str() + " <= " + m[3].str()});
                else
                    out.push_back({a, ""});
            }
        } catch (...) {
        }
    }
    return out;
}

bool auditable(const Finding& f) {
    static const std::set<std::string> stages{"contracts", "wp", "harness", "review"};
    return f.function && stages.count(f.stage) &&
           (laws::is_proof(f.status) || f.status == laws::HYPOTHESIS || f.status == laws::NEEDS_HARNESS ||
            f.status == laws::BOUNDED);
}

}  // namespace

std::vector<Finding> vacuity_audit(const std::vector<FunctionInfo>& functions, const std::vector<Finding>& findings,
                                   const std::string& stage) {
    std::vector<Finding> out;
    std::set<std::string> done;
    for (auto& f : findings) {
        if (!auditable(f)) continue;
        auto* fn = find_fn(functions, f);
        if (!fn) continue;
        auto as = finding_assumptions(f);
        std::vector<std::string> clauses, skipped;
        auto vars = int_params(*fn);
        for (auto& [text, c] : as) {
            if (c.empty()) continue;
            std::string why;
            if (valid_c_bool_expr(c, vars, &why)) clauses.push_back(c);
            else skipped.push_back(text);
        }
        if (clauses.empty()) continue;
        auto key = fn->file + "::" + fn->name + "::" + join(clauses, " && ");
        if (!done.insert(key).second) continue;
        auto r = assumptions_satisfiable(*fn, clauses);
        if (r != "unsat") continue;
        // Name the culprit: a single clause that is unsatisfiable on its own,
        // else the conjunction.
        std::string culprit;
        for (auto& c : clauses)
            if (assumptions_satisfiable(*fn, {c}) == "unsat") {
                culprit = c;
                break;
            }
        auto what = culprit.empty() ? "the conjunction (" + join(clauses, " && ") + ")" : "'" + culprit + "'";
        auto g = base_finding(stage, laws::FAILED, fn, &f, "VACUOUS-ASSUMPTION",
                              "vacuous assumption: " + what + " is unsatisfiable (Z3, parameter widths), so no input "
                              "reaches the body and the " + f.stage + " result " + f.status +
                              " under it proves nothing",
                              laws::STRENGTH_PROVES);
        g.extra["assumptions"] = nlohmann::json(clauses).dump();
        g.extra["audited_stage"] = f.stage;
        g.extra["audited_status"] = f.status;
        g.extra["checker"] = "z3(bitvector encoder)";
        if (!culprit.empty()) g.extra["culprit"] = culprit;
        if (!skipped.empty()) g.extra["not_audited"] = nlohmann::json(skipped).dump();
        out.push_back(std::move(g));
    }
    return out;
}

// Validates {"flags":[{"index":i,"input":s,"reason":s}]} against n assumptions.
Validated validate_assumption_audit(const std::string& raw, std::size_t n) {
    Validated v;
    nlohmann::json j;
    try {
        j = nlohmann::json::parse(trim(raw));
    } catch (...) {
        v.reason = "not JSON";
        return v;
    }
    if (!j.is_object() || j.size() != 1 || !j.contains("flags") || !j["flags"].is_array() || j["flags"].size() > 16) {
        v.reason = "expected {\"flags\": [...]} with at most 16 flags";
        return v;
    }
    for (auto& fl : j["flags"]) {
        if (!fl.is_object() || fl.size() != 3 || !fl.contains("index") || !fl["index"].is_number_integer() ||
            !fl.contains("input") || !fl["input"].is_string() || !fl.contains("reason") || !fl["reason"].is_string()) {
            v.reason = "flag needs integer index, string input and reason";
            v.pairs.clear();
            return v;
        }
        auto idx = fl["index"].get<long long>();
        if (idx < 0 || static_cast<std::size_t>(idx) >= n) {
            v.reason = "flag index out of range";
            v.pairs.clear();
            return v;
        }
        auto input = fl["input"].get<std::string>(), reason = fl["reason"].get<std::string>();
        if (input.size() > 300 || reason.size() > 300 || trim(reason).empty()) {
            v.reason = "flag text empty or too long";
            v.pairs.clear();
            return v;
        }
        v.pairs.push_back({std::to_string(idx), input});
        v.items.push_back(reason);
    }
    v.ok = true;
    return v;
}

std::vector<Finding> model_assumption_audit(const std::vector<FunctionInfo>& functions,
                                            const std::vector<Finding>& findings, const std::string& stage) {
    std::vector<Finding> out;
    struct Item {
        const Finding* f;
        const FunctionInfo* fn;
        std::vector<std::string> assumptions;
    };
    std::vector<Item> items;
    std::set<std::string> seen;
    for (auto& f : findings) {
        if (!auditable(f)) continue;
        auto* fn = find_fn(functions, f);
        if (!fn) continue;
        std::vector<std::string> as;
        for (auto& [text, c] : finding_assumptions(f))
            if (text != "element values unconstrained") as.push_back(text);
        if (as.empty()) continue;
        if (!seen.insert(fn->file + "::" + fn->name + "::" + join(as, "|")).second) continue;
        items.push_back({&f, fn, as});
    }
    if (items.empty()) return out;
    std::string why;
    auto backend = session_backend(&why);
    if (!backend) return out;  // the review stage writes one NOTRUN row for all model features
    int budget = 6;
    for (auto& it : items) {
        if (budget-- <= 0) break;
        std::string list;
        for (std::size_t i = 0; i < it.assumptions.size(); ++i)
            list += std::to_string(i) + ": " + it.assumptions[i] + "\n";
        ModelRequest req;
        req.feature = "assumption-audit";
        req.system = system_prompt(
            "You review the assumptions under which a verifier proved a C function. For each assumption that "
            "could exclude an input the function realistically receives (so the proof would hide a real bug), "
            "return a flag with its index, such an input, and why. Return an empty list when none does.");
        req.user = "Function:\n" + fence_untrusted(function_source(*it.fn), "SOURCE") +
                   "\nAssumptions (index: text), as PRISM recorded them:\n" + list +
                   "Verdict under them: " + it.f->status + " (" + it.f->stage + ")\n";
        req.grammar = "assumption_audit";
        req.grammar_text = grammar_text("assumption_audit");
        AuditRecord rec;
        rec.function = it.fn->name;
        rec.file = it.fn->file;
        rec.checker = "none (flags go to the human reviewer)";
        auto reply = ask(*backend, req, rec);
        if (!reply.error.empty()) {
            rec.checker_result = "no output";
            audit_append(rec);
            continue;
        }
        auto v = validate_assumption_audit(reply.text, it.assumptions.size());
        rec.output_valid = v.ok;
        if (!v.ok) {
            rec.rejected_reason = v.reason;
            rec.checker = "grammar-validator";
            rec.checker_result = "rejected";
            audit_append(rec);
            continue;
        }
        rec.checker_result = "flags: " + std::to_string(v.pairs.size());
        audit_append(rec);
        for (std::size_t k = 0; k < v.pairs.size(); ++k) {
            auto idx = static_cast<std::size_t>(std::stoul(v.pairs[k].first));
            auto g = base_finding(stage, laws::READS, it.fn, it.f, "",
                                  "assumption audit (model, not a verdict): '" + it.assumptions[idx] +
                                      "' may exclude a realistic input (" + v.pairs[k].second + "): " + v.items[k],
                                  laws::STRENGTH_READS);
            g.extra["assumption"] = it.assumptions[idx];
            g.extra["excluded_input"] = v.pairs[k].second;
            g.extra["audited_stage"] = it.f->stage;
            g.extra["audited_status"] = it.f->status;
            g.extra["ai_audit_id"] = rec.id;
            g.extra["verdict_effect"] = "none";
            out.push_back(std::move(g));
        }
    }
    return out;
}

}  // namespace prism::ai
