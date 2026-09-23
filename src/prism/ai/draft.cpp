// Report and assurance-case drafting (roadmap 9.4).
//
// A draft is a list of claims; every claim carries links to what supports
// it: a finding, a verdict, a Lean theorem, a checked certificate or a stage
// row of this run. validate_draft rejects any claim with no link, with a link
// that does not resolve, or whose words claim a proof ("proved", "certified")
// that none of its links supports. Rejected claims are listed separately and
// never rendered as part of the prose.
//
// The deterministic template draft works without a model. A model may reword
// the claims (grammars/draft.gbnf); its draft goes through the same
// validator, so wording can change but support cannot be invented.

#include "ai_internal.hpp"

#include "prism/ai_assist.hpp"
#include "prism/laws.hpp"
#include "prism/pipeline.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>

namespace prism::ai {
namespace fs = std::filesystem;

namespace {
#include "grammars_assist.inc"

std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool is_proof_status(const std::string& s) { return laws::is_proof(s); }

std::string where(const Finding& f) {
    std::string s;
    if (f.function && !f.function->empty()) s += "`" + *f.function + "`";
    if (!f.file.empty()) s += (s.empty() ? "" : " in ") + f.file + (f.line ? ":" + std::to_string(*f.line) : "");
    return s.empty() ? f.stage : s;
}

// Does the text assert a proof / certificate (not negated)?
bool asserts(const std::string& text, const std::vector<std::string>& words) {
    auto t = lower(text);
    for (auto& w : words) {
        for (std::size_t p = t.find(w); p != std::string::npos; p = t.find(w, p + 1)) {
            if (p > 0 && std::isalpha(static_cast<unsigned char>(t[p - 1]))) {
                // "unproved": the prefix negates; any other letter means another word
                if (p >= 2 && t.compare(p - 2, 2, "un") == 0) continue;
                continue;
            }
            bool negated = false;
            for (const char* neg : {"not ", "no ", "never ", "without ", "cannot be ", "not yet ", "not a ", "never a ", "no a "})
                if (p >= std::string(neg).size() && t.compare(p - std::string(neg).size(), std::string(neg).size(), neg) == 0)
                    negated = true;
            if (!negated) return true;
        }
    }
    return false;
}

}  // namespace

std::vector<std::string> lean_theorems(const fs::path& proofs_dir) {
    std::vector<std::string> out;
    std::error_code ec;
    if (proofs_dir.empty() || !fs::is_directory(proofs_dir, ec)) return out;
    fs::recursive_directory_iterator it(proofs_dir, fs::directory_options::skip_permission_denied, ec), end;
    for (; !ec && it != end; it.increment(ec)) {
        if (it->is_directory(ec)) {
            auto n = it->path().filename().string();
            if (n == ".lake" || n == "build" || n.starts_with(".")) it.disable_recursion_pending();
            continue;
        }
        if (it->path().extension() != ".lean") continue;
        std::ifstream in(it->path());
        std::vector<std::string> stack;  // namespace names ("" for sections)
        for (std::string line; std::getline(in, line);) {
            std::istringstream is(line);
            std::string kw, name;
            is >> kw;
            // attributes / modifiers before the keyword
            while (kw == "private" || kw == "protected" || kw == "noncomputable" || kw.starts_with("@[")) {
                if (!(is >> kw)) break;
            }
            if (kw == "namespace" && (is >> name)) {
                stack.push_back(name);
            } else if (kw == "section") {
                stack.push_back("");
            } else if (kw == "end") {
                if (!stack.empty()) stack.pop_back();
            } else if ((kw == "theorem" || kw == "lemma") && (is >> name)) {
                std::string full;
                for (auto& s : stack)
                    if (!s.empty()) full += s + ".";
                out.push_back(full + name);
            }
        }
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

Draft draft_template(const RunReport& report, const std::string& kind, const std::vector<std::string>& theorems) {
    Draft d;
    d.kind = kind == "assurance" ? "assurance" : "report";
    d.author = "template";
    auto refs = enumerate_findings(report);

    // --- scope
    DraftSection scope{"Scope", {}};
    {
        std::set<std::string> files;
        for (auto& fn : report.functions) files.insert(fn.file);
        std::ostringstream t;
        t << "PRISM " << PRISM_VERSION << " analysed " << report.root << ": " << report.functions.size()
          << " functions in " << files.size() << " files, " << report.stages.size() << " stages.";
        Claim c{t.str(), {}};
        for (auto& s : report.stages)
            if (s.name == "inventory") c.links.push_back("stage:inventory");
        scope.claims.push_back(c);
        std::vector<std::string> notrun;
        for (auto& s : report.stages)
            if (s.status == laws::NOTRUN) notrun.push_back(s.name);
        if (!notrun.empty()) {
            Claim g;
            g.text = std::to_string(notrun.size()) + " stages could not run (NOTRUN, a gap, not a clean result): ";
            for (std::size_t i = 0; i < notrun.size(); ++i) {
                g.text += (i ? ", " : "") + notrun[i];
                g.links.push_back("stage:" + notrun[i]);
            }
            g.text += ".";
            scope.claims.push_back(g);
        }
        std::ostringstream conf;
        conf << "Confidence " << report.confidence << " = visibility " << report.visibility << " x answer "
             << report.answer << " x resolution " << report.resolution << " (Law 5).";
        Claim cc{conf.str(), {}};
        for (auto& s : report.stages)
            if (s.name == "unify") cc.links.push_back("stage:unify");
        scope.claims.push_back(cc);
    }
    d.sections.push_back(scope);

    // --- defects, ordered by the triage clusters
    DraftSection defects{"Defects found", {}};
    {
        auto tri = triage(report, TriageOptions{0.55, false, {}});
        int n = 0;
        for (auto& c : tri.clusters) {
            if (c.top_status != "FAILED" && c.top_status != "CRASH" && c.top_status != "SANFAIL") continue;
            if (++n > 40) break;
            const Finding* rep = find_by_id(report, c.representative);
            if (!rep) continue;
            Claim cl;
            cl.text = rep->status + ": " + (rep->cls.empty() ? rep->message.substr(0, 80) : rep->cls) + " at " +
                      where(*rep);
            if (!rep->counterexample.empty()) cl.text += ", counterexample " + rep->counterexample.substr(0, 80);
            if (c.stages.size() > 1) {
                cl.text += " (reported by " + std::to_string(c.stages.size()) + " stages: ";
                for (std::size_t i = 0; i < c.stages.size(); ++i) cl.text += (i ? ", " : "") + c.stages[i];
                cl.text += ")";
            }
            cl.text += ".";
            cl.links.push_back("verdict:" + c.representative);
            for (std::size_t i = 1; i < c.members.size() && i < 8; ++i) cl.links.push_back("finding:" + c.members[i]);
            defects.claims.push_back(cl);
        }
        if (defects.claims.empty()) {
            // A "no defects" claim needs support: the defect-producing stages that ran.
            Claim none{"No FAILED, CRASH or SANFAIL finding was reported by the stages that ran; this is not a "
                       "proof of absence (see the gaps).",
                       {}};
            for (auto& s : report.stages)
                if (s.status != laws::NOTRUN && (s.name == "bmc" || s.name == "pir" || s.name == "fuzz" || s.name == "sanitize"))
                    none.links.push_back("stage:" + s.name);
            defects.claims.push_back(none);
        }
    }
    d.sections.push_back(defects);

    // --- proofs
    DraftSection proofs{"Proofs", {}};
    {
        std::map<std::string, std::vector<const FindingRef*>> by;
        for (auto& r : refs)
            if (is_proof_status(r.f->status) && r.f->function) by[r.f->status].push_back(&r);
        for (const char* st : {"PROVED-CERTIFIED", "PROVED-UNBOUNDED", "PROVED", "PROVED-ASSUMING"}) {
            auto it = by.find(st);
            if (it == by.end()) continue;
            Claim c;
            std::set<std::string> fns;
            for (auto* r : it->second) fns.insert(r->f->file + ":" + r->f->function.value_or(""));
            c.text = std::to_string(fns.size()) + " functions are " + st;
            if (std::string(st) == "PROVED-ASSUMING") c.text += " (proved only under the listed assumptions)";
            if (std::string(st) == "PROVED") c.text += " (the solver is trusted; no certificate)";
            if (std::string(st) == "PROVED-UNBOUNDED") c.text += " (all loop iterations)";
            c.text += ", e.g. ";
            int k = 0;
            for (auto* r : it->second) {
                if (k < 5) c.text += (k ? ", " : "") + where(*r->f);
                ++k;
                c.links.push_back("verdict:" + r->id);
                if (std::string(st) == "PROVED-CERTIFIED") c.links.push_back("certificate:" + r->id);
            }
            c.text += ".";
            proofs.claims.push_back(c);
        }
        if (proofs.claims.empty()) {
            Claim c{"No proof verdict was reached in this run.", {}};
            for (auto& s : report.stages)
                if (s.name == "bmc" || s.name == "pir") c.links.push_back("stage:" + s.name);
            proofs.claims.push_back(c);
        }
    }
    d.sections.push_back(proofs);

    // --- gaps
    DraftSection gaps{"Gaps and limits", {}};
    {
        for (const char* st : {"BOUNDED", "NEEDS-HARNESS", "UNKNOWN", "TIMEOUT"}) {
            Claim c;
            int n = 0;
            for (auto& r : refs)
                if (r.f->status == st && r.f->function) {
                    ++n;
                    if (c.links.size() < 60) c.links.push_back("finding:" + r.id);
                }
            if (!n) continue;
            c.text = std::to_string(n) + " results are " + st;
            if (std::string(st) == "BOUNDED") c.text += ": checked only up to the unwind bound, not proved";
            if (std::string(st) == "NEEDS-HARNESS") c.text += ": pointer/struct functions not model-checked without a harness (Law 6)";
            c.text += ".";
            gaps.claims.push_back(c);
        }
    }
    if (!gaps.claims.empty()) d.sections.push_back(gaps);

    // --- trusted base / argument (assurance case)
    if (d.kind == "assurance") {
        DraftSection arg{"Argument", {}};
        Claim top{"Top claim: every verdict above holds to the strength of its status, relative to the trusted base "
                  "(docs/TRUSTED_BASE.md).",
                  {}};
        for (auto& s : report.stages)
            if (s.name == "unify") top.links.push_back("stage:unify");
        arg.claims.push_back(top);
        auto has = [&](const std::string& t) { return std::find(theorems.begin(), theorems.end(), t) != theorems.end(); };
        struct Step {
            const char* text;
            std::vector<std::string> thms;
        };
        const std::vector<Step> steps = {
            {"A BOUNDED result is never merged with a proof, so no bounded check is reported as proved.",
             {"Prism.proved_bounded_never_merge", "Prism.formal_never_merge"}},
            {"Model output (LLM) can never be admitted as a proof; it stays HYPOTHESIS until a checker accepts it.",
             {"Prism.admit_model_never_proof", "Prism.no_path_fuzzer_or_model_to_proof"}},
            {"A fuzzer's CLEAN result is never promoted to a proof.",
             {"Prism.admit_fuzzer_never_proof", "Prism.clean_never_promoted"}},
            {"PROVED-CERTIFIED is admitted only with a checked certificate.",
             {"Prism.certified_only_with_certificate", "Prism.admit_certified_iff"}},
            {"A NOTRUN result never becomes a clean result.", {"Prism.notrun_never_becomes_clean", "Prism.reach_notrun"}}};
        for (auto& s : steps) {
            Claim c{s.text, {}};
            for (auto& t : s.thms)
                if (has(t)) c.links.push_back("theorem:" + t);
            // No theorem found (proofs/ not available): the step stays unlinked
            // and the validator rejects it rather than letting it read as proved.
            arg.claims.push_back(c);
        }
        d.sections.push_back(arg);
    }
    return d;
}

DraftCheck validate_draft(const Draft& d, const RunReport& report, const std::vector<std::string>& theorems) {
    DraftCheck chk;
    std::set<std::string> stages;
    for (auto& s : report.stages) stages.insert(s.name);
    std::set<std::string> thm(theorems.begin(), theorems.end());
    for (auto& sec : d.sections) {
        for (auto& c : sec.claims) {
            ++chk.claims;
            auto reject = [&](const std::string& why) {
                chk.ok = false;
                chk.rejected.push_back(sec.title + ": " + c.text.substr(0, 120) + ": " + why);
            };
            if (c.links.empty()) {
                reject("no link (every claim must link to a finding, verdict, theorem or certificate)");
                continue;
            }
            bool bad = false, proof_support = false, cert_support = false;
            for (auto& l : c.links) {
                ++chk.links;
                auto colon = l.find(':');
                auto kind = colon == std::string::npos ? "" : l.substr(0, colon);
                auto ref = colon == std::string::npos ? "" : l.substr(colon + 1);
                if (kind == "finding" || kind == "verdict" || kind == "certificate") {
                    const Finding* f = find_by_id(report, ref);
                    if (!f) {
                        reject("link " + l + " does not resolve");
                        bad = true;
                        break;
                    }
                    if (kind == "verdict" && !laws::is_answered(f->status) && f->status != laws::CRASH &&
                        f->status != laws::SANFAIL) {
                        reject("link " + l + " is " + f->status + ", not a verdict");
                        bad = true;
                        break;
                    }
                    if (kind == "certificate" &&
                        (f->status != laws::PROVED_CERTIFIED || f->extra.count(std::string(laws::CERTIFICATE_KEY)) == 0 ||
                         f->extra.at(std::string(laws::CERTIFICATE_KEY)) != laws::CERTIFICATE_CHECKED)) {
                        reject("link " + l + " has no checked certificate");
                        bad = true;
                        break;
                    }
                    if (kind != "finding" && is_proof_status(f->status)) proof_support = true;
                    if (kind == "certificate") cert_support = true;
                } else if (kind == "theorem") {
                    if (!thm.contains(ref)) {
                        reject("theorem " + ref + " is not declared under proofs/");
                        bad = true;
                        break;
                    }
                    proof_support = true;
                    cert_support = true;  // a theorem may speak about certification
                } else if (kind == "stage") {
                    if (!stages.contains(ref)) {
                        reject("stage " + ref + " is not in this report");
                        bad = true;
                        break;
                    }
                } else {
                    reject("unknown link kind '" + l + "'");
                    bad = true;
                    break;
                }
            }
            if (bad) continue;
            if (asserts(c.text, {"certified"}) && !cert_support)
                reject("claims a certificate but links none");
            else if (asserts(c.text, {"proved", "proven", "proof ", "proves "}) && !proof_support &&
                     !asserts(c.text, {"proved only under"}))
                reject("claims a proof but links no proof verdict or theorem");
        }
    }
    return chk;
}

std::string render_draft(const Draft& d, const DraftCheck& check) {
    std::set<std::string> rejected_text;
    for (auto& r : check.rejected) rejected_text.insert(r);
    std::ostringstream o;
    o << "# PRISM " << (d.kind == "assurance" ? "assurance case" : "report") << " (draft)\n\n";
    o << "Author: " << d.author << ". Every claim links to its support (finding, verdict, theorem, certificate or "
      << "stage of report.json); " << check.claims << " claims, " << check.links << " links, "
      << check.rejected.size() << " rejected. Roadmap 9.4.\n";
    for (auto& sec : d.sections) {
        std::ostringstream body;
        for (auto& c : sec.claims) {
            // Skip rejected claims: they are listed at the end instead.
            bool rej = false;
            for (auto& r : check.rejected) rej = rej || r.starts_with(sec.title + ": " + c.text.substr(0, 120) + ": ");
            if (rej) continue;
            body << "- " << c.text;
            std::size_t shown = 0;
            for (auto& l : c.links) {
                if (shown++ >= 8) {
                    body << " [+" << (c.links.size() - 8) << " more]";
                    break;
                }
                body << " [" << l << "]";
            }
            body << "\n";
        }
        if (body.str().empty()) continue;
        o << "\n## " << sec.title << "\n\n" << body.str();
    }
    if (!check.rejected.empty()) {
        o << "\n## Rejected claims (not part of the draft)\n\n";
        for (auto& r : check.rejected) o << "- " << r << "\n";
    }
    return o.str();
}

Draft draft_with_model(const RunReport& report, const std::string& kind, const std::vector<std::string>& theorems,
                       std::string* note) {
    Draft base = draft_template(report, kind, theorems);
    std::string why;
    auto backend = session_config() ? session_backend(&why) : nullptr;
    if (!session_config()) why = "no AI session";
    if (!backend) {
        if (note) *note = "NOTRUN: " + why + " (template draft)";
        return base;
    }
    nlohmann::json claims = nlohmann::json::array();
    for (auto& s : base.sections)
        for (auto& c : s.claims) claims.push_back({{"section", s.title}, {"text", c.text}, {"links", c.links}});
    ModelRequest req;
    req.feature = "draft";
    req.grammar = "draft";
    req.grammar_text = GBNF_DRAFT;
    req.system = system_prompt(
        "Task: rewrite these report claims as clear prose sentences for a reviewer. Keep each claim's links "
        "exactly as given; never add a claim without links; never say proved or certified unless a linked "
        "verdict is a proof. Output the JSON list described by the grammar.");
    req.user = fence_untrusted(claims.dump(1), "CLAIMS");
    AuditRecord rec;
    rec.feature = "draft";
    rec.grammar = "draft";
    auto rep = ask(*backend, req, rec);
    Draft d;
    d.kind = base.kind;
    d.author = "llm:" + backend->name();
    std::string reject;
    try {
        if (!rep.error.empty()) throw std::runtime_error(rep.error);
        auto j = nlohmann::json::parse(trim(rep.text));
        if (!j.is_array() || j.empty() || j.size() > 64) throw std::runtime_error("expected a list of 1..64 claims");
        std::map<std::string, std::size_t> idx;
        for (auto& c : j) {
            if (!c.is_object() || !c.contains("section") || !c.contains("text") || !c.contains("links") ||
                !c["section"].is_string() || !c["text"].is_string() || !c["links"].is_array())
                throw std::runtime_error("claim must be {section, text, links}");
            auto sec = c["section"].get<std::string>();
            if (!idx.contains(sec)) {
                idx[sec] = d.sections.size();
                d.sections.push_back({sec, {}});
            }
            Claim cl{c["text"].get<std::string>(), {}};
            for (auto& l : c["links"]) {
                if (!l.is_string()) throw std::runtime_error("links must be strings");
                cl.links.push_back(l.get<std::string>());
            }
            d.sections[idx[sec]].claims.push_back(cl);
        }
    } catch (const std::exception& ex) {
        reject = ex.what();
    }
    DraftCheck chk;
    if (reject.empty()) chk = validate_draft(d, report, theorems);
    rec.output_valid = reject.empty();
    rec.rejected_reason = reject;
    rec.checker = "draft-link-validator";
    rec.checker_result = !reject.empty() ? "rejected" : chk.ok ? "accepted" : "partially-rejected";
    rec.verdict_effect = "none";
    audit_append(rec);
    if (!reject.empty()) {
        if (note) *note = "model draft rejected (" + reject + "); template draft used";
        return base;
    }
    if (note) *note = "model draft validated (" + std::to_string(chk.rejected.size()) + " claims rejected; audit " + rec.id + ")";
    return d;
}

int draft_main(int argc, char** argv) {
    fs::path report_path = "prism-out/report.json", proofs, out;
    std::string kind = "report";
    bool use_model = true;
    for (int i = 0; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--report") {
            fs::path p(next());
            report_path = fs::is_directory(p) ? p / "report.json" : p;
        } else if (a == "--kind") kind = next();
        else if (a == "--proofs") proofs = next();
        else if (a == "--out") out = next();
        else if (a == "--no-llm") use_model = false;
        else if (a == "-h" || a == "--help") {
            std::cout << "prism draft [--report OUT/report.json] [--kind report|assurance] [--proofs DIR] [--out FILE]\n"
                         "            [--no-llm]\n"
                         "Drafts report / assurance-case prose where every claim links to a finding, verdict,\n"
                         "theorem (proofs/**/*.lean) or checked certificate; unlinked claims are rejected.\n"
                         "Writes FILE (default <report dir>/draft_<kind>.md) and the claims as .json.\n";
            return 0;
        }
    }
    if (kind != "report" && kind != "assurance") {
        std::cerr << "--kind expects report|assurance\n";
        return 2;
    }
    auto report = RunReport::load(report_path);
    if (!report) {
        std::cerr << "ERROR draft: cannot read " << report_path.string() << "\n";
        return 2;
    }
    if (proofs.empty()) {
        for (fs::path cand : {fs::path(report->root) / "proofs", fs::path("proofs")}) {
            std::error_code ec;
            if (fs::is_directory(cand, ec)) {
                proofs = cand;
                break;
            }
        }
    }
    auto thms = lean_theorems(proofs);
    Config cfg = default_config();
    cfg.llm = use_model;
    cfg.resume = true;
    cfg.out = fs::absolute(report_path).parent_path();
    cfg.root = report->root;
    Session session(cfg);
    std::string note;
    Draft d = use_model ? draft_with_model(*report, kind, thms, &note) : draft_template(*report, kind, thms);
    if (!use_model) note = "--no-llm (template draft)";
    auto chk = validate_draft(d, *report, thms);
    if (out.empty()) out = cfg.out / ("draft_" + kind + ".md");
    std::ofstream(out, std::ios::binary) << render_draft(d, chk);
    nlohmann::json j;
    j["kind"] = d.kind;
    j["author"] = d.author;
    j["note"] = note;
    j["theorems_indexed"] = thms.size();
    j["ok"] = chk.ok;
    j["rejected"] = chk.rejected;
    j["sections"] = nlohmann::json::array();
    for (auto& s : d.sections) {
        nlohmann::json cs = nlohmann::json::array();
        for (auto& c : s.claims) cs.push_back({{"text", c.text}, {"links", c.links}});
        j["sections"].push_back({{"title", s.title}, {"claims", cs}});
    }
    auto jout = out;
    jout.replace_extension(".json");
    std::ofstream(jout, std::ios::binary) << j.dump(2);
    std::cout << "draft " << out.string() << "  (" << chk.claims << " claims, " << chk.links << " links, "
              << chk.rejected.size() << " rejected; " << note << "; " << thms.size() << " theorems indexed)\n";
    for (auto& r : chk.rejected) std::cout << "  rejected: " << r << "\n";
    return 0;
}

}  // namespace prism::ai
