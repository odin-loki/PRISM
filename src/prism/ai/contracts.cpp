// Contract drafting (roadmap 4.2) and contracts from requirements (9.3).
//
// For a SCALAR function with no specification, the model drafts requires /
// ensures clauses (grammar contract.gbnf, identifiers narrowed to the
// parameters). Each clause carries a trace link: a requirement sentence
// (R<n>, from --requirements markdown/text files), the function's comments,
// or the code itself. PRISM then
//   * rejects a draft whose requires is unsatisfiable (Z3 vacuity check),
//   * proves the function against the draft (the contracts engine: BMC with
//     requires assumed and ensures asserted at every return),
//   * checks every caller in the tree satisfies the requires at each call
//     (modular: the call is replaced by a fresh value, the requires is
//     asserted with the arguments substituted).
// A drafted contract stays HYPOTHESIS until a human approves it: the
// approval file (contracts.approved.json) lists each clause by
// sha256(function "\n" clause)[:16]. Only approved contracts are proved as
// PROVED-ASSUMING (and a caller that violates an approved requires is FAILED).

#include "ai_internal.hpp"

#include "prism/ai_proof.hpp"
#include "prism/laws.hpp"
#include "prism/stages.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <map>
#include <regex>
#include <set>
#include <sstream>

namespace prism::ai {
namespace fs = std::filesystem;

std::vector<std::string> split_conjuncts(const std::string& expr);  // assumption_audit.cpp

// ------------------------------------------------------------------ requirements
std::vector<Requirement> load_requirements(const std::vector<fs::path>& paths) {
    std::vector<Requirement> out;
    std::vector<fs::path> files;
    std::error_code ec;
    for (auto& p : paths) {
        if (fs::is_directory(p, ec)) {
            for (auto& e : fs::recursive_directory_iterator(p, fs::directory_options::skip_permission_denied, ec)) {
                auto ext = e.path().extension().string();
                if (e.is_regular_file(ec) && (ext == ".md" || ext == ".txt" || ext == ".rst" || ext == ".markdown"))
                    files.push_back(e.path());
            }
        } else if (fs::is_regular_file(p, ec)) {
            files.push_back(p);
        }
    }
    std::sort(files.begin(), files.end());
    static const std::regex marker(R"(^\s*(?:#{1,6}\s+|[-*+]\s+|\d+[.)]\s+|>\s*)+)");
    for (auto& f : files) {
        std::ifstream in(f);
        std::string line;
        int n = 0;
        bool fence = false;
        while (std::getline(in, line) && out.size() < 2000) {
            ++n;
            auto t = trim(line);
            if (t.rfind("```", 0) == 0) {
                fence = !fence;
                continue;
            }
            if (fence || t.empty()) continue;
            t = std::regex_replace(t, marker, "");
            // Sentences: split after . ! ? followed by a space.
            std::size_t start = 0;
            for (std::size_t i = 0; i <= t.size(); ++i) {
                bool end = i == t.size() ||
                           ((t[i] == '.' || t[i] == '!' || t[i] == '?') && i + 1 < t.size() && t[i + 1] == ' ');
                if (!end) continue;
                auto s = trim(t.substr(start, i - start + (i < t.size() ? 1 : 0)));
                start = i + 1;
                int words = 0;
                bool in_word = false;
                for (char c : s) {
                    bool w = !std::isspace(static_cast<unsigned char>(c));
                    if (w && !in_word) ++words;
                    in_word = w;
                }
                if (words < 3) continue;
                Requirement r;
                r.id = "R" + std::to_string(out.size() + 1);
                r.file = f.string();
                r.line = n;
                r.text = s.substr(0, 400);
                out.push_back(std::move(r));
            }
        }
    }
    return out;
}

// ------------------------------------------------------------------ approvals
namespace {
std::string normalise_clause(const std::string& clause) {
    std::string out;
    bool space = false;
    for (char c : trim(clause)) {
        if (std::isspace(static_cast<unsigned char>(c))) {
            space = true;
            continue;
        }
        if (space && !out.empty()) out += ' ';
        space = false;
        out += c;
    }
    return out;
}

struct Approved {
    std::string function, file, clause, hash;
};

std::vector<Approved> read_approved(const fs::path& path) {
    std::vector<Approved> out;
    std::ifstream in(path);
    if (!in) return out;
    nlohmann::json j;
    try {
        in >> j;
    } catch (...) {
        return out;
    }
    auto arr = j.is_object() && j.contains("approved") ? j["approved"] : j;
    if (!arr.is_array()) return out;
    for (auto& e : arr) {
        if (!e.is_object()) continue;
        Approved a;
        a.function = e.value("function", "");
        a.file = e.value("file", "");
        a.clause = normalise_clause(e.value("clause", ""));
        a.hash = e.value("hash", "");
        // A stale or hand-edited entry (hash of another clause) is not an approval.
        if (a.function.empty() || a.clause.empty() || a.hash != clause_hash(a.function, a.clause)) continue;
        out.push_back(std::move(a));
    }
    return out;
}
}  // namespace

std::string clause_hash(const std::string& function, const std::string& clause) {
    return sha256_hex(function + "\n" + normalise_clause(clause)).substr(0, 16);
}

std::vector<std::string> load_approvals(const fs::path& path) {
    std::vector<std::string> out;
    for (auto& a : read_approved(path)) out.push_back(a.hash);
    return out;
}

// ------------------------------------------------------------------ syntax
std::string contract_to_c(const std::string& expr_in) {
    std::string e = trim(expr_in);
    for (std::size_t p; (p = e.find("\\result")) != std::string::npos;) e.replace(p, 7, "result");
    // Top-level (right-associative) implication.
    int depth = 0;
    for (std::size_t i = 0; i + 2 < e.size(); ++i) {
        if (e[i] == '(') ++depth;
        else if (e[i] == ')') --depth;
        else if (depth == 0 && e.compare(i, 3, "==>") == 0) {
            auto lhs = trim(e.substr(0, i)), rhs = trim(e.substr(i + 3));
            return "(!(" + contract_to_c(lhs) + ") || (" + contract_to_c(rhs) + "))";
        }
    }
    // Implications inside parentheses.
    std::string out;
    for (std::size_t i = 0; i < e.size(); ++i) {
        if (e[i] != '(') {
            out += e[i];
            continue;
        }
        int d = 0;
        std::size_t j = i;
        for (; j < e.size(); ++j) {
            if (e[j] == '(') ++d;
            else if (e[j] == ')' && --d == 0) break;
        }
        if (j >= e.size()) {
            out += e.substr(i);
            break;
        }
        out += "(" + contract_to_c(e.substr(i + 1, j - i - 1)) + ")";
        i = j;
    }
    return out;
}

namespace {

bool is_ident_char(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }

// Replace whole-word identifiers per `map`.
std::string substitute(const std::string& expr, const std::map<std::string, std::string>& map) {
    std::string out;
    for (std::size_t i = 0; i < expr.size();) {
        if (std::isalpha(static_cast<unsigned char>(expr[i])) || expr[i] == '_') {
            std::size_t j = i;
            while (j < expr.size() && is_ident_char(expr[j])) ++j;
            auto w = expr.substr(i, j - i);
            auto it = map.find(w);
            out += it != map.end() ? it->second : w;
            i = j;
            continue;
        }
        out += expr[i++];
    }
    return out;
}

bool simple_arg(const std::string& a) {
    if (a.empty() || a.size() > 200) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        char c = a[i];
        if (!(is_ident_char(c) || std::strchr(" +-*/%()", c))) return false;
        if ((c == '+' || c == '-') && i + 1 < a.size() && a[i + 1] == c) return false;
        if (c == '(' && i > 0) {
            auto k = i;
            while (k > 0 && a[k - 1] == ' ') --k;
            if (k > 0 && is_ident_char(a[k - 1])) return false;  // a call
        }
    }
    return true;
}

}  // namespace

std::string check_callers_requires(const FunctionInfo& caller, const FunctionInfo& callee,
                                   const std::string& requires_expr, int unwind) {
    const auto& body = caller.body;
    std::regex call("\\b" + callee.name + "\\s*\\(");
    struct Site {
        std::size_t stmt, begin, end;
        std::vector<std::string> args;
    };
    std::vector<Site> sites;
    for (auto it = std::sregex_iterator(body.begin(), body.end(), call); it != std::sregex_iterator(); ++it) {
        auto b = static_cast<std::size_t>(it->position());
        if (b > 0 && (body[b - 1] == '.' || body[b - 1] == '>')) continue;
        auto open = b + static_cast<std::size_t>(it->length()) - 1;
        int d = 0;
        std::size_t close = open;
        for (; close < body.size(); ++close) {
            if (body[close] == '(') ++d;
            else if (body[close] == ')' && --d == 0) break;
        }
        if (close >= body.size()) return "unknown: unbalanced call";
        std::vector<std::string> args;
        {
            int dd = 0;
            std::size_t s = open + 1;
            for (auto k = open + 1; k <= close; ++k) {
                char c = body[k];
                if (c == '(') ++dd;
                else if ((c == ')' && dd-- == 0) || (c == ',' && dd == 0)) {
                    auto a = trim(body.substr(s, k - s));
                    if (!a.empty()) args.push_back(a);
                    s = k + 1;
                }
            }
        }
        if (args.size() != callee.params.size()) return "unknown: argument count differs from the signature";
        for (auto& a : args)
            if (!simple_arg(a)) return "unknown: argument '" + a.substr(0, 40) + "' is not side-effect free";
        // Statement start: after the last ; { } before the call.
        std::size_t st = body.find_last_of(";{}", b);
        st = st == std::string::npos ? 0 : st + 1;
        auto prefix = trim(body.substr(st, b - st));
        auto stmt_end = body.find_first_of(";{", close);
        auto whole = body.substr(st, (stmt_end == std::string::npos ? body.size() : stmt_end) - st);
        if (whole.find("&&") != std::string::npos || whole.find("||") != std::string::npos ||
            whole.find('?') != std::string::npos)
            return "unknown: call under a short-circuit or conditional operator";
        static const std::regex loop_kw(R"(^(for|while|do|else|switch|case|default)\b)");
        if (std::regex_search(prefix, loop_kw)) return "unknown: call in a loop header or else/switch arm";
        if (prefix.rfind("if", 0) == 0) {
            int bal = 0;
            for (char c : prefix) bal += c == '(' ? 1 : c == ')' ? -1 : 0;
            if (bal <= 0) return "unknown: call in the unbraced body of an if";
        }
        sites.push_back({st, b, close + 1, args});
    }
    if (sites.empty()) return "no-calls";
    std::string nb = body;
    for (std::size_t k = sites.size(); k-- > 0;) {
        auto& s = sites[k];
        std::map<std::string, std::string> sub;
        for (std::size_t i = 0; i < callee.params.size(); ++i) sub[callee.params[i].second] = "(" + s.args[i] + ")";
        auto req = substitute(requires_expr, sub);
        auto tag = std::to_string(k + 1);
        auto ret = "__prism_ret" + tag;
        nb.replace(s.begin, s.end - s.begin, ret);
        nb.insert(s.stmt, " int " + ret + " = 0; __prism_assert(" + tag + ", " + req + "); __prism_havoc(" + ret + "); ");
    }
    auto pc = check_program(caller, nb, unwind, 6000, /*only_invariants=*/true);
    if (!pc.encoded) return "unknown: " + (pc.error.empty() ? std::string("caller not encoded") : pc.error);
    std::string verdict = "satisfies";
    for (auto& p : pc.props) {
        if (p.name.rfind("ai-inv#", 0) != 0) continue;
        if (p.result == "sat") return "violates: " + (p.model.empty() ? std::string("counterexample") : p.model);
        if (p.result != "unsat") verdict = "unknown: solver " + p.result;
    }
    if (verdict == "satisfies" && !pc.unwind_ok) verdict = "satisfies-bounded (loops not closed within the unwind)";
    return verdict;
}

// ------------------------------------------------------------------ drafting
namespace {

bool has_spec(const FunctionInfo& fn) {
    static const std::regex spec(R"((//|/\*|\*|@)\s*(requires|ensures|invariant|decreases)\b)");
    return std::regex_search(function_source(fn), spec);
}

struct Clause {
    std::string kind, expr, c, trace, hash;
};

Finding review_finding(std::string_view status, const FunctionInfo& fn, std::string cls, std::string msg,
                       std::string_view strength) {
    Finding f;
    f.stage = "review";
    f.status = std::string(status);
    f.file = fn.file;
    f.function = fn.name;
    f.line = fn.line;
    f.cls = std::move(cls);
    f.message = std::move(msg);
    f.strength = std::string(strength);
    return f;
}

std::string contract_text(const std::vector<Clause>& cs) {
    std::string s;
    for (auto& c : cs) {
        if (!s.empty()) s += " ";
        s += c.kind + " " + c.expr + ";";
    }
    return s;
}

// Proves fn against the clauses; fills extras shared by drafted and approved rows.
Finding prove_clauses(const FunctionInfo& fn, const std::vector<Clause>& cs, int unwind, std::string* req_out) {
    std::vector<std::string> req, ens;
    for (auto& c : cs) (c.kind == "requires" ? req : ens).push_back("(" + c.c + ")");
    std::optional<std::string> r, e;
    if (!req.empty()) r = join(req, " && ");
    if (!ens.empty()) e = join(ens, " && ");
    if (req_out) *req_out = r ? *r : "";
    return prove_with_contract(fn, unwind, r, e);
}

std::string callers_json(const std::vector<FunctionInfo>& functions, const FunctionInfo& fn, const std::string& req,
                         int unwind, std::vector<std::pair<const FunctionInfo*, std::string>>* results) {
    nlohmann::json j = nlohmann::json::object();
    if (req.empty()) return j.dump();
    int budget = 8;
    std::regex call("\\b" + fn.name + "\\s*\\(");
    for (auto& g : functions) {
        if (&g == &fn || (g.name == fn.name && g.file == fn.file)) continue;
        if (!std::regex_search(g.body, call)) continue;
        if (budget-- <= 0) {
            j["..."] = "more callers not checked (budget 8)";
            break;
        }
        auto r = check_callers_requires(g, fn, req, unwind);
        j[g.file + ":" + g.name] = r;
        if (results) results->push_back({&g, r});
    }
    return j.dump();
}

}  // namespace

std::vector<Finding> draft_contracts(const std::vector<FunctionInfo>& functions, const Config& cfg) {
    std::vector<Finding> out;
    const int unwind = cfg.unwind;
    auto approval_path = !cfg.contracts_approved.empty()
                             ? cfg.contracts_approved
                             : (fs::is_directory(cfg.root) ? cfg.root : cfg.root.parent_path()) / "contracts.approved.json";
    auto approved = read_approved(approval_path);

    // 1. Approved contracts (no model needed): proved, callers checked.
    std::set<std::string> has_approved;
    std::map<std::string, std::vector<Approved>> by_fn;
    for (auto& a : approved) by_fn[a.function].push_back(a);
    for (auto& fn : functions) {
        auto it = by_fn.find(fn.name);
        if (it == by_fn.end()) continue;
        std::vector<Clause> cs;
        for (auto& a : it->second) {
            if (!a.file.empty() && a.file != fn.file) continue;
            Clause c;
            auto sp = a.clause.find(' ');
            c.kind = a.clause.substr(0, sp);
            c.expr = trim(a.clause.substr(sp + 1));
            if (!c.expr.empty() && c.expr.back() == ';') c.expr.pop_back();
            std::vector<std::string> vars;
            for (auto& p : fn.params) vars.push_back(p.second);
            auto v = validate_contract(c.kind + " " + c.expr + ";", vars);
            if ((c.kind != "requires" && c.kind != "ensures") || !v.ok) continue;
            c.c = contract_to_c(c.expr);
            c.hash = a.hash;
            cs.push_back(c);
        }
        if (cs.empty()) continue;
        has_approved.insert(fn.file + "::" + fn.name);
        std::string req;
        auto r = prove_clauses(fn, cs, unwind, &req);
        std::string status = r.status;
        std::string msg;
        if (laws::is_proof(status)) {
            status = std::string(laws::PROVED_ASSUMING);
            msg = "approved contract holds (" + contract_text(cs) + "); PROVED-ASSUMING the approved requires";
        } else if (status == laws::FAILED) {
            msg = "violates its approved contract (" + contract_text(cs) + "): " + r.message;
        } else {
            msg = "approved contract (" + contract_text(cs) + "): " + status + " — " + r.message;
        }
        auto f = review_finding(status, fn, "FUNC-CONTRACT", msg, laws::STRENGTH_PROVES);
        f.counterexample = r.counterexample;
        nlohmann::json cj = nlohmann::json::array();
        for (auto& c : cs) cj.push_back({{"kind", c.kind}, {"expr", c.expr}, {"hash", c.hash}});
        f.extra["contract"] = cj.dump();
        f.extra["contract_state"] = "approved";
        f.extra["approval_file"] = approval_path.string();
        f.extra["requires"] = req;
        f.extra["checker"] = "contracts engine (bmc)";
        f.extra["checker_status"] = r.status;
        std::vector<std::pair<const FunctionInfo*, std::string>> callers;
        f.extra["callers"] = callers_json(functions, fn, req, unwind, &callers);
        out.push_back(std::move(f));
        for (auto& [g, res] : callers) {
            if (res.rfind("violates", 0) != 0) continue;
            auto h = review_finding(laws::FAILED, *g, "FUNC-CONTRACT",
                                    "call of " + fn.name + " violates its approved requires (" + req + "): " + res,
                                    laws::STRENGTH_PROVES);
            h.counterexample = res.substr(std::min<std::size_t>(res.size(), 10));
            h.extra["callee"] = fn.name;
            h.extra["requires"] = req;
            out.push_back(std::move(h));
        }
    }

    // 2. Drafting (model): spec-less SCALAR functions, requirement-mentioned first.
    auto reqs = load_requirements(cfg.requirements);
    std::vector<const FunctionInfo*> cands;
    for (auto& fn : functions) {
        if (fn.kind != "SCALAR" || has_approved.count(fn.file + "::" + fn.name) || has_spec(fn)) continue;
        cands.push_back(&fn);
    }
    auto mentions = [&](const FunctionInfo& fn) {
        std::vector<const Requirement*> hit;
        std::regex word("\\b" + fn.name + "\\b", std::regex::icase);
        for (auto& r : reqs)
            if (std::regex_search(r.text, word)) hit.push_back(&r);
        return hit;
    };
    std::stable_sort(cands.begin(), cands.end(), [&](auto* a, auto* b) {
        return mentions(*a).size() > mentions(*b).size();
    });
    if (cands.empty()) return out;
    std::string why;
    auto backend = session_backend(&why);
    if (!backend) return out;  // the review stage writes one NOTRUN row for the model features
    int budget = 8;
    for (auto* fnp : cands) {
        if (budget-- <= 0) break;
        auto& fn = *fnp;
        std::vector<std::string> vars;
        for (auto& p : fn.params) vars.push_back(p.second);
        auto hits = mentions(fn);
        if (hits.size() > 30) hits.resize(30);
        std::string reqtxt;
        for (auto* r : hits) reqtxt += r->id + " (" + r->file + ":" + std::to_string(r->line) + "): " + r->text + "\n";
        ModelRequest req;
        req.feature = "contract";
        req.system = system_prompt(
            "You draft a C function contract: `requires <expr>;` clauses over the parameters and `ensures "
            "<expr>;` clauses over the parameters and \\result, one per line, each ending with a trace "
            "` // from R<n>` (the requirement sentence it comes from), ` // from comment` or ` // from code`. "
            "Only state what the requirements, the comments or the code support.");
        req.user = "Function:\n" + fence_untrusted(function_source(fn), "SOURCE") +
                   (reqtxt.empty() ? std::string("\nNo requirement sentence mentions this function.\n")
                                   : "\nRequirement sentences (id (file:line): text):\n" +
                                         fence_untrusted(reqtxt, "REQUIREMENTS")) +
                   "\nParameters: " + join(vars, ", ") + "\n";
        req.grammar = "contract";
        req.grammar_text = grammar_for("contract", vars);
        AuditRecord rec;
        rec.function = fn.name;
        rec.file = fn.file;
        rec.checker = "contracts engine (bmc) + z3 vacuity + caller check";
        auto reply = ask(*backend, req, rec);
        if (!reply.error.empty()) {
            rec.checker_result = "no output";
            audit_append(rec);
            continue;
        }
        auto v = validate_contract(reply.text, vars);
        std::map<std::string, const Requirement*> offered;
        for (auto* r : hits) offered[r->id] = r;
        if (v.ok) {
            for (auto& t : v.traces)
                if (!t.empty() && t[0] == 'R' && !offered.count(t)) {
                    v.ok = false;
                    v.reason = "trace " + t + " names a requirement that was not offered";
                }
        }
        rec.output_valid = v.ok;
        if (!v.ok) {
            rec.rejected_reason = v.reason;
            rec.checker = "grammar-validator";
            rec.checker_result = "rejected";
            audit_append(rec);
            continue;
        }
        std::vector<Clause> cs;
        for (std::size_t i = 0; i < v.pairs.size(); ++i) {
            Clause c;
            c.kind = v.pairs[i].first;
            c.expr = v.pairs[i].second;
            c.c = contract_to_c(c.expr);
            c.trace = i < v.traces.size() ? v.traces[i] : "";
            c.hash = clause_hash(fn.name, c.kind + " " + c.expr + ";");
            cs.push_back(c);
        }
        std::vector<std::string> reqc;
        for (auto& c : cs)
            if (c.kind == "requires") reqc.push_back(c.c);
        auto sat = reqc.empty() ? std::string("sat") : assumptions_satisfiable(fn, reqc);
        if (sat == "unsat") {
            rec.checker = "z3 vacuity";
            rec.checker_result = "rejected: requires unsatisfiable";
            audit_append(rec);
            auto f = review_finding(laws::HYPOTHESIS, fn, "VACUOUS-ASSUMPTION",
                                    "drafted contract rejected: its requires is unsatisfiable (" +
                                        contract_text(cs) + ")",
                                    laws::STRENGTH_READS);
            f.extra["ai_audit_id"] = rec.id;
            out.push_back(std::move(f));
            continue;
        }
        std::string reqs_c;
        auto r = prove_clauses(fn, cs, unwind, &reqs_c);
        auto callers = callers_json(functions, fn, reqs_c, unwind, nullptr);
        rec.checker_result = r.status;
        rec.verdict_effect = "none";  // HYPOTHESIS until a human approves every clause
        audit_append(rec);
        nlohmann::json cj = nlohmann::json::array(), approve = nlohmann::json::array(), trace = nlohmann::json::array();
        for (auto& c : cs) {
            nlohmann::json t{{"clause", c.kind + " " + c.expr + ";"}, {"source", c.trace.empty() ? "unstated" : c.trace}};
            if (auto it = offered.find(c.trace); it != offered.end()) {
                t["file"] = it->second->file;
                t["line"] = it->second->line;
                t["text"] = it->second->text;
            } else if (c.trace == "comment" || c.trace == "code") {
                t["file"] = fn.file;
                t["line"] = fn.line;
            }
            trace.push_back(t);
            cj.push_back({{"kind", c.kind}, {"expr", c.expr}, {"hash", c.hash}, {"trace", c.trace}});
            approve.push_back({{"function", fn.name}, {"file", fn.file}, {"clause", c.kind + " " + c.expr + ";"},
                               {"hash", c.hash}});
        }
        auto f = review_finding(laws::HYPOTHESIS, fn, "FUNC-CONTRACT",
                                "drafted contract (HYPOTHESIS until a human approves it): " + contract_text(cs) +
                                    " — contracts engine: " + r.status,
                                laws::STRENGTH_READS);
        f.counterexample = r.counterexample;
        f.extra["contract"] = cj.dump();
        f.extra["contract_state"] = "drafted";
        f.extra["trace"] = trace.dump();
        f.extra["proof_status"] = r.status;
        f.extra["proof_message"] = r.message;
        f.extra["requires"] = reqs_c;
        f.extra["callers"] = callers;
        f.extra["approve_with"] = approve.dump();
        f.extra["approval_file"] = approval_path.string();
        f.extra["ai_audit_id"] = rec.id;
        f.extra["verdict_effect"] = "none";
        out.push_back(std::move(f));
    }
    return out;
}

}  // namespace prism::ai
