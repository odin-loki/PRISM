// Proof store and proof repair (roadmap 9.2).
//
// After the solver stages, every function with a proof-class result is
// recorded in <out>/proof_store.json (and, when $PRISM_PROOF_CACHE is set,
// in a persistent per-repository copy): a fingerprint of its code and the
// artefacts that proved it (loop invariants, contract requires/ensures,
// drafted harness assumptions, the unwind).
//
// On the next run, a stored function the current run does not prove is
// re-checked with its stored artefacts (Houdini + loop-cut induction for
// invariants, the contracts engine for contracts, the drafted harness BMC):
//   * still checks -> the proof is re-established from the store (the checker
//     decided, so the verdict is the checker's);
//   * does not check -> PROOF-REGRESSION (UNKNOWN, extra.regression = true)
//     naming what was proved before, and why it no longer holds. With a
//     model, repairs are proposed (new invariants seeded with the old ones;
//     an updated contract) and re-checked: a repaired invariant proof is
//     accepted by Houdini + induction; a repaired contract is a new
//     specification, so it stays HYPOTHESIS until a human approves it.
// Detection needs no model.

#include "ai_internal.hpp"

#include "prism/ai_proof.hpp"
#include "prism/laws.hpp"
#include "prism/stages.hpp"
#include "prism/verdict.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <map>
#include <set>

namespace prism::ai {
namespace fs = std::filesystem;

std::string function_fingerprint(const FunctionInfo& fn) {
    // Comments and whitespace do not change the proof obligation.
    std::string code;
    const std::string src = fn.signature + "\n" + fn.body;
    for (std::size_t i = 0; i < src.size(); ++i) {
        if (src.compare(i, 2, "//") == 0) {
            while (i < src.size() && src[i] != '\n') ++i;
            continue;
        }
        if (src.compare(i, 2, "/*") == 0) {
            auto e = src.find("*/", i + 2);
            i = e == std::string::npos ? src.size() : e + 1;
            continue;
        }
        if (!std::isspace(static_cast<unsigned char>(src[i]))) code += src[i];
    }
    return sha256_hex(code);
}

std::vector<ProofEntry> load_proof_store(const fs::path& path) {
    std::vector<ProofEntry> out;
    std::ifstream in(path);
    if (!in) return out;
    nlohmann::json j;
    try {
        in >> j;
    } catch (...) {
        return out;
    }
    if (!j.is_object() || !j.contains("functions") || !j["functions"].is_array()) return out;
    for (auto& e : j["functions"]) {
        ProofEntry p;
        p.file = e.value("file", "");
        p.function = e.value("function", "");
        p.fingerprint = e.value("fingerprint", "");
        if (e.contains("artefacts") && e["artefacts"].is_array())
            for (auto& a : e["artefacts"]) {
                ProofArtefact x;
                x.stage = a.value("stage", "");
                x.status = a.value("status", "");
                x.invariants = a.value("invariants", "");
                x.requires_ = a.value("requires", "");
                x.ensures = a.value("ensures", "");
                x.assumptions = a.value("assumptions", "");
                x.source = a.value("source", "");
                x.unwind = a.value("unwind", 0);
                p.artefacts.push_back(std::move(x));
            }
        if (!p.function.empty() && !p.artefacts.empty()) out.push_back(std::move(p));
    }
    return out;
}

void save_proof_store(const fs::path& path, const std::vector<ProofEntry>& entries) {
    nlohmann::json fns = nlohmann::json::array();
    for (auto& e : entries) {
        nlohmann::json arts = nlohmann::json::array();
        for (auto& a : e.artefacts)
            arts.push_back({{"stage", a.stage},
                            {"status", a.status},
                            {"invariants", a.invariants},
                            {"requires", a.requires_},
                            {"ensures", a.ensures},
                            {"assumptions", a.assumptions},
                            {"source", a.source},
                            {"unwind", a.unwind}});
        fns.push_back({{"file", e.file}, {"function", e.function}, {"fingerprint", e.fingerprint}, {"artefacts", arts}});
    }
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream(path, std::ios::binary | std::ios::trunc)
        << nlohmann::json{{"version", 1}, {"functions", fns}}.dump(2) << "\n";
}

fs::path persistent_store_path(const fs::path& root) {
    fs::path dir;
    if (const char* v = std::getenv("PRISM_PROOF_CACHE"); v && *v) dir = v;
    else if (const char* x = std::getenv("XDG_CACHE_HOME"); x && *x) dir = fs::path(x) / "prism" / "proofs";
    else if (const char* h = std::getenv("HOME"); h && *h) dir = fs::path(h) / ".cache" / "prism" / "proofs";
    else return {};
    std::error_code ec;
    auto canon = fs::weakly_canonical(root, ec);
    return dir / (sha256_hex((ec ? root : canon).string()).substr(0, 16) + ".json");
}

namespace {

Finding row(std::string_view status, const FunctionInfo& fn, std::string cls, std::string msg, std::string_view strength) {
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

std::optional<std::vector<std::vector<std::string>>> parse_invariants(const std::string& s) {
    try {
        auto j = nlohmann::json::parse(s);
        if (!j.is_array()) return std::nullopt;
        std::vector<std::vector<std::string>> out;
        for (auto& loop : j) {
            if (!loop.is_array()) return std::nullopt;
            std::vector<std::string> v;
            for (auto& x : loop)
                if (x.is_string()) v.push_back(x.get<std::string>());
            out.push_back(std::move(v));
        }
        return out;
    } catch (...) {
        return std::nullopt;
    }
}

std::string describe(const ProofArtefact& a) {
    if (!a.invariants.empty()) return "loop invariants " + a.invariants;
    if (!a.requires_.empty() || !a.ensures.empty())
        return "contract requires (" + a.requires_ + ") ensures (" + a.ensures + ")";
    if (!a.assumptions.empty()) return "harness assumptions " + a.assumptions;
    return "bounded model checking at unwind " + std::to_string(a.unwind);
}

int proof_rank(const std::string& status) {
    auto v = verdict::parse(status);
    return v ? verdict::rank(*v) : 0;
}

}  // namespace

Finding recheck_artefact(const FunctionInfo& fn, const ProofArtefact& a, int unwind) {
    if (!a.invariants.empty()) {
        auto inv = parse_invariants(a.invariants);
        std::string why;
        auto loops = loop_cuts(fn, &why);
        if (!inv || !loops)
            return row(laws::UNKNOWN, fn, "", "stored invariants cannot be re-applied: " + (inv ? why : "bad JSON"),
                       laws::STRENGTH_PROVES);
        if (loops->size() != inv->size())
            return row(laws::UNKNOWN, fn, "",
                       "loop structure changed (" + std::to_string(inv->size()) + " stored invariant sets, " +
                           std::to_string(loops->size()) + " loops now)",
                       laws::STRENGTH_PROVES);
        std::vector<std::vector<std::string>> srcs;
        for (auto& v : *inv) srcs.push_back(std::vector<std::string>(v.size(), a.source.empty() ? "store" : a.source));
        auto h = houdini(fn, *loops, *inv, srcs, unwind);
        if (h.proved) {
            auto f = row(laws::PROVED_UNBOUNDED, fn, "",
                         "stored loop invariants still close the loop-cut induction (Houdini + k-induction, Z3)",
                         laws::STRENGTH_PROVES);
            nlohmann::json j = h.invariants;
            f.extra["invariants"] = j.dump();
            f.extra["k_induction"] = "closed-invariants";
            f.extra["unwind_closed"] = "false";
            return f;
        }
        auto f = row(laws::BOUNDED, fn, "", "stored invariants no longer inductive: " + (h.why.empty() ? "step open" : h.why),
                     laws::STRENGTH_PROVES);
        f.counterexample = h.cti;
        return f;
    }
    if (!a.requires_.empty() || !a.ensures.empty()) {
        std::optional<std::string> r, e;
        if (!a.requires_.empty()) r = a.requires_;
        if (!a.ensures.empty()) e = a.ensures;
        auto f = prove_with_contract(fn, unwind, r, e);
        f.stage = "review";
        return f;
    }
    if (!a.assumptions.empty()) {
        std::string refused;
        auto d = drafted_harness_bmc(fn, unwind, &refused);
        if (!d) return row(laws::UNKNOWN, fn, "", "harness can no longer be drafted: " + refused, laws::STRENGTH_PROVES);
        auto f = *d;
        f.stage = "review";
        if (laws::is_proof(f.status) && f.extra["assumptions"] != a.assumptions) {
            // A proof under different assumptions is a different claim.
            f.extra["assumptions_changed"] = "true";
            f.extra["previous_assumptions"] = a.assumptions;
        }
        return f;
    }
    auto recs = run_bmc({fn}, unwind);
    auto f = recs.empty() ? row(laws::ERROR, fn, "", "bmc returned nothing", laws::STRENGTH_PROVES) : recs[0];
    f.stage = "review";
    return f;
}

namespace {

// Model repair of invariants: the stored invariants and the new code go to
// the model; Houdini + loop-cut induction decide.
std::optional<Finding> repair_invariants(const FunctionInfo& fn, const ProofArtefact& a, int unwind,
                                         ModelBackend& backend, std::string* audit_id) {
    std::string why;
    auto loops = loop_cuts(fn, &why);
    if (!loops || loops->empty()) return std::nullopt;
    std::vector<std::string> vocab;
    for (auto& L : *loops)
        for (auto& v : L.scalars)
            if (std::find(vocab.begin(), vocab.end(), v) == vocab.end()) vocab.push_back(v);
    ModelRequest req;
    req.feature = "proof-repair";
    req.system = system_prompt(
        "A loop invariant proof of this C function broke when the code changed. Propose loop invariants "
        "(C boolean expressions over the listed variables) that hold for the NEW code; the previous "
        "invariants are given as a starting point.");
    req.user = "Changed function:\n" + fence_untrusted(function_source(fn), "SOURCE") +
               "\nPrevious invariants (per loop): " + a.invariants + "\nVariables: " + join(vocab, ", ") + "\n";
    req.grammar = "invariants";
    req.grammar_text = grammar_for("invariants", vocab);
    AuditRecord rec;
    rec.function = fn.name;
    rec.file = fn.file;
    rec.checker = "z3-houdini+k-induction";
    auto reply = ask(backend, req, rec);
    auto v = reply.error.empty() ? validate_invariants(reply.text, vocab) : Validated{};
    rec.output_valid = v.ok;
    if (!v.ok) {
        rec.rejected_reason = reply.error.empty() ? v.reason : reply.error;
        rec.checker = "grammar-validator";
        rec.checker_result = "rejected";
        audit_append(rec);
        return std::nullopt;
    }
    // Every loop gets the stored survivors, templates and the model's candidates.
    auto stored = parse_invariants(a.invariants);
    std::vector<std::vector<std::string>> cands, srcs;
    for (std::size_t j = 0; j < loops->size(); ++j) {
        std::vector<std::string> c, s;
        if (stored && j < stored->size())
            for (auto& x : (*stored)[j]) c.push_back(x), s.push_back("store");
        for (auto& x : template_candidates(fn, (*loops)[j])) c.push_back(x), s.push_back("template");
        for (auto& x : v.items) c.push_back(x), s.push_back("llm:" + backend.name());
        cands.push_back(c);
        srcs.push_back(s);
    }
    auto h = houdini(fn, *loops, cands, srcs, unwind);
    rec.checker_result = h.proved ? std::string(laws::PROVED_UNBOUNDED) : "step-open";
    rec.verdict_effect = h.proved ? std::string(laws::PROVED_UNBOUNDED) : "none";
    audit_append(rec);
    if (audit_id) *audit_id = rec.id;
    if (!h.proved) return std::nullopt;
    auto f = row(laws::PROVED_UNBOUNDED, fn, "",
                 "proof repaired: new loop invariants (model-proposed, Houdini + loop-cut induction in Z3) prove the "
                 "changed function",
                 laws::STRENGTH_PROVES);
    nlohmann::json j = h.invariants;
    f.extra["invariants"] = j.dump();
    f.extra["invariant_source"] = "llm:" + backend.name();
    f.extra["k_induction"] = "closed-invariants";
    f.extra["unwind_closed"] = "false";
    f.extra["ai_audit_id"] = rec.id;
    f.extra["ai_checker"] = "z3-houdini+k-induction";
    f.extra["ai_checker_result"] = std::string(laws::PROVED_UNBOUNDED);
    return f;
}

// Model repair of a contract: an updated contract is a new specification,
// so it is re-proved but stays HYPOTHESIS until approved.
std::optional<Finding> repair_contract(const FunctionInfo& fn, const ProofArtefact& a, int unwind,
                                       ModelBackend& backend) {
    std::vector<std::string> vars;
    for (auto& p : fn.params) vars.push_back(p.second);
    ModelRequest req;
    req.feature = "proof-repair";
    req.system = system_prompt(
        "A contract proof of this C function broke when the code changed. Propose an updated contract "
        "(`requires <expr>;` / `ensures <expr>;` lines, \\result for the return value) for the NEW code.");
    req.user = "Changed function:\n" + fence_untrusted(function_source(fn), "SOURCE") + "\nPrevious contract: requires (" +
               a.requires_ + ") ensures (" + a.ensures + ")\n";
    req.grammar = "contract";
    req.grammar_text = grammar_for("contract", vars);
    AuditRecord rec;
    rec.function = fn.name;
    rec.file = fn.file;
    rec.checker = "contracts engine (bmc)";
    auto reply = ask(backend, req, rec);
    auto v = reply.error.empty() ? validate_contract(reply.text, vars) : Validated{};
    rec.output_valid = v.ok;
    if (!v.ok) {
        rec.rejected_reason = reply.error.empty() ? v.reason : reply.error;
        rec.checker = "grammar-validator";
        rec.checker_result = "rejected";
        audit_append(rec);
        return std::nullopt;
    }
    std::vector<std::string> req_c, ens_c, text;
    for (auto& [k, e] : v.pairs) {
        (k == "requires" ? req_c : ens_c).push_back("(" + contract_to_c(e) + ")");
        text.push_back(k + " " + e + ";");
    }
    std::optional<std::string> r, e;
    if (!req_c.empty()) r = join(req_c, " && ");
    if (!ens_c.empty()) e = join(ens_c, " && ");
    auto chk = prove_with_contract(fn, unwind, r, e);
    rec.checker_result = chk.status;
    rec.verdict_effect = "none";
    audit_append(rec);
    auto f = row(laws::HYPOTHESIS, fn, "FUNC-CONTRACT",
                 "proposed contract repair (HYPOTHESIS until approved): " + join(text, " ") +
                     " — contracts engine: " + chk.status,
                 laws::STRENGTH_READS);
    nlohmann::json approve = nlohmann::json::array();
    for (auto& t : text) approve.push_back({{"function", fn.name}, {"file", fn.file}, {"clause", t}, {"hash", clause_hash(fn.name, t)}});
    f.extra["approve_with"] = approve.dump();
    f.extra["proof_status"] = chk.status;
    f.extra["ai_audit_id"] = rec.id;
    f.extra["verdict_effect"] = "none";
    return f;
}

const std::set<std::string>& proving_stages() {
    static const std::set<std::string> s{"bmc", "contracts", "harness", "wp", "pir", "review"};
    return s;
}

}  // namespace

std::vector<Finding> run_proof_regression(const std::vector<FunctionInfo>& functions,
                                          const std::vector<StageResult>& stages, const Config& cfg) {
    std::vector<Finding> out;
    auto store_path = cfg.out / "proof_store.json";
    auto persistent = std::getenv("PRISM_PROOF_CACHE") ? persistent_store_path(cfg.root) : fs::path{};
    std::error_code ec;
    auto previous = load_proof_store(store_path);
    if (previous.empty() && !persistent.empty() && fs::exists(persistent, ec)) previous = load_proof_store(persistent);

    auto key_of = [](const std::string& file, const std::string& fn) { return file + "::" + fn; };
    std::map<std::string, const FunctionInfo*> fns;
    for (auto& fn : functions) fns.emplace(key_of(fn.file, fn.name), &fn);

    // Proofs of this run.
    std::map<std::string, ProofEntry> current;
    auto record = [&](const Finding& f) {
        if (!f.function || !laws::is_proof(f.status)) return;
        auto k = key_of(f.file, *f.function);
        auto it = fns.find(k);
        if (it == fns.end()) return;
        auto& e = current[k];
        e.file = f.file;
        e.function = *f.function;
        e.fingerprint = function_fingerprint(*it->second);
        ProofArtefact a;
        a.stage = f.stage;
        a.status = f.status;
        auto get = [&](const char* key) {
            auto x = f.extra.find(key);
            return x == f.extra.end() ? std::string{} : x->second;
        };
        a.invariants = get("invariants");
        if (a.invariants == "[]") a.invariants.clear();
        if (f.stage == "contracts" || f.stage == "wp" || get("contract_state") == "approved") {
            a.requires_ = get("requires");
            a.ensures = get("ensures");
        }
        if (f.stage == "harness") a.assumptions = get("assumptions");
        a.source = !get("invariant_source").empty() ? get("invariant_source")
                   : !get("harness_source").empty() ? get("harness_source")
                                                    : "pipeline";
        a.unwind = cfg.unwind;
        for (auto& old : e.artefacts)
            if (old.stage == a.stage && old.invariants == a.invariants && old.requires_ == a.requires_ &&
                old.ensures == a.ensures && old.assumptions == a.assumptions)
                return;
        e.artefacts.push_back(std::move(a));
    };
    for (auto& s : stages)
        if (proving_stages().count(s.name))
            for (auto& f : s.findings) record(f);

    std::string why_no_model;
    std::shared_ptr<ModelBackend> backend;
    bool asked_backend = false;
    int repair_budget = 6;

    for (auto& prev : previous) {
        auto k = key_of(prev.file, prev.function);
        auto fit = fns.find(k);
        if (fit == fns.end()) continue;  // function removed or renamed: nothing to keep proved
        auto& fn = *fit->second;
        auto fp = function_fingerprint(fn);
        bool changed = fp != prev.fingerprint;
        int best_prev = 0;
        std::string prev_status, prev_stage;
        for (auto& a : prev.artefacts)
            if (proof_rank(a.status) > best_prev) {
                best_prev = proof_rank(a.status);
                prev_status = a.status;
                prev_stage = a.stage;
            }
        if (auto cit = current.find(k); cit != current.end()) {
            int best_now = 0;
            for (auto& a : cit->second.artefacts) best_now = std::max(best_now, proof_rank(a.status));
            if (best_now >= best_prev) continue;  // proof maintained (or improved) by this run
        }
        // Re-check the stored artefacts, strongest first.
        auto arts = prev.artefacts;
        std::stable_sort(arts.begin(), arts.end(),
                         [](auto& a, auto& b) { return proof_rank(a.status) > proof_rank(b.status); });
        std::optional<Finding> reproved;
        std::vector<std::string> failures;
        for (auto& a : arts) {
            auto r = recheck_artefact(fn, a, cfg.unwind);
            if (laws::is_proof(r.status)) {
                r.stage = "review";
                r.message = "re-proved from the proof store (" + a.stage + " " + describe(a) + "): " + r.message;
                r.extra["proof_store"] = "reused";
                r.extra["code_changed"] = changed ? "true" : "false";
                r.extra["previous_status"] = a.status;
                r.extra["previous_stage"] = a.stage;
                if (r.status == laws::PROVED_ASSUMING || !a.requires_.empty()) r.extra["requires"] = a.requires_;
                reproved = std::move(r);
                break;
            }
            failures.push_back(a.stage + " " + describe(a) + " -> " + r.status + ": " + r.message.substr(0, 300));
        }
        if (reproved) {
            record(*reproved);
            out.push_back(std::move(*reproved));
            continue;
        }
        // Repair with the model (optional), re-checked before acceptance.
        std::optional<Finding> repaired;
        std::vector<Finding> proposals;
        if (!asked_backend) {
            asked_backend = true;
            backend = session_backend(&why_no_model);
        }
        if (backend && repair_budget > 0) {
            for (auto& a : arts) {
                if (repair_budget-- <= 0) break;
                std::string aid;
                if (!a.invariants.empty()) {
                    if (auto r = repair_invariants(fn, a, cfg.unwind, *backend, &aid)) {
                        repaired = std::move(r);
                        break;
                    }
                } else if (!a.requires_.empty() || !a.ensures.empty()) {
                    if (auto r = repair_contract(fn, a, cfg.unwind, *backend)) proposals.push_back(std::move(*r));
                }
            }
        }
        if (repaired) {
            repaired->extra["regression"] = "repaired";
            repaired->extra["previous_status"] = prev_status;
            repaired->extra["previous_stage"] = prev_stage;
            repaired->extra["code_changed"] = changed ? "true" : "false";
            record(*repaired);
            out.push_back(std::move(*repaired));
            continue;
        }
        auto f = row(laws::UNKNOWN, fn, "PROOF-REGRESSION",
                     "proof regression: " + fn.name + " was " + prev_status + " (" + prev_stage + ")" +
                         (changed ? "; the function changed and " : "; the code is unchanged but ") +
                         "the stored proof artefacts no longer check: " + (failures.empty() ? "none stored" : failures[0]),
                     laws::STRENGTH_PROVES);
        f.extra["regression"] = "true";
        f.extra["previous_status"] = prev_status;
        f.extra["previous_stage"] = prev_stage;
        f.extra["previous_fingerprint"] = prev.fingerprint;
        f.extra["fingerprint"] = fp;
        f.extra["code_changed"] = changed ? "true" : "false";
        f.extra["cause"] = changed ? "code changed" : "unchanged code (tool, configuration, unwind or timeout)";
        f.extra["rechecks"] = nlohmann::json(failures).dump();
        f.extra["repair"] = backend ? (proposals.empty() ? "model proposals did not re-check" : "contract proposal (HYPOTHESIS)")
                                    : "NOTRUN: " + why_no_model;
        out.push_back(std::move(f));
        for (auto& p : proposals) out.push_back(std::move(p));
        // Keep the old entry so the regression is reported again until fixed.
        current.emplace(k, prev);
    }

    std::vector<ProofEntry> entries;
    for (auto& [k, e] : current) entries.push_back(e);
    save_proof_store(store_path, entries);
    if (!persistent.empty()) save_proof_store(persistent, entries);
    return out;
}

}  // namespace prism::ai
