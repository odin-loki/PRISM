#include "report.hpp"

#include "prism/regex.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <set>

namespace prism::qa {

namespace {

// Python str() of a JSON value read from a report
std::string jstr(const ojson& j) {
    if (j.is_string()) return j.get<std::string>();
    if (j.is_null()) return "None";
    if (j.is_boolean()) return j.get<bool>() ? "True" : "False";
    if (j.is_number_float()) return py_float(j.get<double>());
    return j.dump();
}

// Python truthiness of a JSON value
bool truthy(const ojson& j) {
    if (j.is_null()) return false;
    if (j.is_boolean()) return j.get<bool>();
    if (j.is_string()) return !j.get<std::string>().empty();
    if (j.is_number_integer()) return j.get<long long>() != 0;
    if (j.is_number_unsigned()) return j.get<unsigned long long>() != 0;
    if (j.is_number_float()) return j.get<double>() != 0.0;
    if (j.is_array() || j.is_object()) return !j.empty();
    return false;
}

// f.get(key) of a finding (null when absent)
const ojson& field(const ojson& f, const char* key) {
    static const ojson null;
    if (!f.is_object()) return null;
    auto it = f.find(key);
    return it == f.end() ? null : *it;
}

std::string status_of(const ojson& f) { return jstr(field(f, "status")); }

// f.get("extra", {}).get(key)
const ojson& extra(const ojson& f, const char* key) {
    static const ojson null;
    const ojson& e = field(f, "extra");
    if (!e.is_object()) return null;
    auto it = e.find(key);
    return it == e.end() ? null : *it;
}

std::set<std::string> statuses(const ojson& found) {
    std::set<std::string> s;
    for (const auto& f : found) s.insert(status_of(f));
    return s;
}

bool any_proof(const std::set<std::string>& st) {
    for (const auto& p : PROOF)
        if (st.count(p)) return true;
    return false;
}

}  // namespace

std::string classify(const Task& task, const std::string& fn, const ojson& found) {
    const std::set<std::string> st = statuses(found);
    const bool expected = task.expected_of(fn);
    const bool prop_scoped = PROPERTY_SCOPED.count(task.origin) > 0;  // labels speak for one property only
    static const std::set<std::string> none;
    auto pc = PROPERTY_CLASSES.find(task.prop);
    const std::set<std::string>& classes = pc == PROPERTY_CLASSES.end() ? none : pc->second;
    std::vector<const ojson*> failed, failed_in_prop;
    for (const auto& f : found)
        if (status_of(f) == "FAILED") failed.push_back(&f);
    auto cls_of = [](const ojson& f) -> std::optional<std::string> {
        const ojson& c = field(f, "cls");
        if (c.is_string()) return c.get<std::string>();
        return std::nullopt;
    };
    for (const ojson* f : failed) {
        auto c = cls_of(*f);
        if (!prop_scoped || (c && classes.count(*c))) failed_in_prop.push_back(f);
    }
    if (!expected && task.expect_class.count(fn)) {
        // the refutation must be for the violation the task plants
        const auto& want = task.expect_class.at(fn);
        std::vector<const ojson*> keep;
        for (const ojson* f : failed_in_prop) {
            auto c = cls_of(*f);
            if (c && want.count(*c)) keep.push_back(f);
        }
        failed_in_prop = keep;
    }
    if (st.empty()) return "missing";
    if (!expected && any_proof(st)) return "wrong-proof";
    if (auto it = task.expect_status.find(fn); it != task.expect_status.end()) {
        const std::string& want = it->second;
        if (!failed.empty() && expected) return "false-alarm";
        if (any_proof(st) || !failed.empty()) return want == "NEEDS-HARNESS" ? "law6-violation" : "unexpected-status";
        return st.count(want) ? "law-ok" : "unexpected-status";
    }
    if (expected && !failed_in_prop.empty()) return "false-alarm";
    if (expected && !failed.empty()) return "failed-other-property";
    if (expected && any_proof(st)) return "proved";
    if (!expected && !failed_in_prop.empty()) return "refuted";
    if (!expected && !failed.empty()) return "failed-other-property";
    if (st.count("BOUNDED")) return "bounded";
    return "no-answer";
}

std::string pct(long a, long b) {
    if (!b) return "n/a";
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.1f%%", 100.0 * static_cast<double>(a) / static_cast<double>(b));
    return buf;
}

double py_round(double x, int n) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.*f", n, x);
    return std::strtod(buf, nullptr);
}

ojson compute_metrics(const std::vector<ojson>& rows, const std::vector<std::string>& stages) {
    ojson metrics = ojson::object();
    std::set<std::string> oset;
    for (const auto& r : rows) oset.insert(r["origin"].get<std::string>());
    std::vector<std::string> origins(oset.begin(), oset.end());
    origins.push_back("all");
    static const std::vector<std::string> kinds = {"wrong-proof",  "false-alarm", "proved",  "refuted",
                                                   "bounded",      "no-answer",   "missing", "failed-other-property",
                                                   "law-ok",       "law6-violation", "unexpected-status"};
    for (const auto& stage : stages) {
        metrics[stage] = ojson::object();
        for (const auto& origin : origins) {
            std::vector<const ojson*> sel;
            for (const auto& r : rows)
                if (r["stage"] == stage && (origin == "all" || r["origin"] == origin)) sel.push_back(&r);
            long true_n = 0, false_n = 0, law_n = 0, replayed = 0;
            std::map<std::string, long> c;
            for (const auto& k : kinds) c[k] = 0;
            for (const ojson* r : sel) {
                const bool law = (*r)["law_task"].get<bool>();
                const bool exp = (*r)["expected"].get<bool>();
                if (!law) (exp ? true_n : false_n)++;
                if (law) ++law_n;
                const std::string oc = (*r)["outcome"].get<std::string>();
                if (c.count(oc)) c[oc]++;
                if (oc == "refuted" && r->contains("replay") && (*r)["replay"].value("replay", "") == "replayed")
                    ++replayed;
            }
            ojson m = ojson::object();
            m["functions"] = static_cast<long>(sel.size());
            m["true"] = true_n;
            m["false"] = false_n;
            m["wrong_proofs"] = c["wrong-proof"];
            m["false_alarms"] = c["false-alarm"];
            m["proved"] = c["proved"];
            m["refuted"] = c["refuted"];
            m["refuted_replayed"] = replayed;
            m["bounded"] = c["bounded"];
            m["no_answer"] = c["no-answer"] + c["missing"];
            m["failed_other_property"] = c["failed-other-property"];
            m["law_tasks"] = law_n;
            m["law_ok"] = c["law-ok"];
            m["law6_violations"] = c["law6-violation"] + c["unexpected-status"];
            m["soundness_ok"] = c["wrong-proof"] == 0;
            auto ratio = [](long a, long b) -> ojson {
                if (!b) return nullptr;
                return py_round(static_cast<double>(a) / static_cast<double>(b), 4);
            };
            m["completeness"] = ratio(c["proved"], true_n);
            m["detection"] = ratio(replayed, false_n);
            m["detection_unreplayed"] = ratio(c["refuted"], false_n);
            metrics[stage][origin] = m;
        }
    }
    return metrics;
}

ojson certified_summary(const std::vector<ojson>& rows) {
    auto st = [](const ojson& r) { return statuses(r["findings"]); };
    auto ex = [](const ojson& r, const char* key) -> std::string {
        for (const auto& f : r["findings"])
            if (truthy(extra(f, key))) return jstr(extra(f, key));
        return "";
    };
    std::vector<const ojson*> sel, loop_free, looped, unencoded;
    for (const auto& r : rows)
        if (r["stage"] == CERT_STAGE && r["expected"].get<bool>() && !r["law_task"].get<bool>()) sel.push_back(&r);
    for (const ojson* r : sel) {
        const ojson* loops = nullptr;
        for (const auto& f : (*r)["findings"])
            if (truthy(extra(f, "loops"))) {
                loops = &extra(f, "loops");
                break;
            }
        if (loops && loops->is_string() && loops->get<std::string>() == "0") loop_free.push_back(r);
        else if (loops) looped.push_back(r);
        else unencoded.push_back(r);
    }
    std::vector<const ojson*> cert, proved;
    for (const ojson* r : loop_free) {
        auto s = st(*r);
        if (s.count("PROVED-CERTIFIED")) cert.push_back(r);
        if (any_proof(s)) proved.push_back(r);
    }
    auto in = [](const std::vector<const ojson*>& v, const ojson* r) {
        return std::find(v.begin(), v.end(), r) != v.end();
    };
    // One certificate for the whole function (certificate_scope = combined)
    // or one per VC; either certifies every VC of the function.
    std::vector<const ojson*> all_cert;
    for (const auto& r : rows)
        if (r["stage"] == CERT_STAGE && st(r).count("PROVED-CERTIFIED")) all_cert.push_back(&r);
    // A function with no VC stays PROVED (a certificate that checks nothing is
    // not a certificate): counted apart, not as "proved, not certified".
    std::vector<const ojson*> no_vcs;
    for (const ojson* r : proved)
        if (!in(cert, r) && ex(*r, "certificate_vcs") == "0") no_vcs.push_back(r);
    ojson not_cert = ojson::array();
    for (const ojson* r : proved) {
        if (in(cert, r) || in(no_vcs, r)) continue;
        ojson x = ojson::object();
        x["task"] = (*r)["task"];
        x["function"] = (*r)["function"];
        x["note"] = ex(*r, "certify_note");
        not_cert.push_back(x);
    }
    static const Regex share_re(R"(^(\d+)/(\d+))");
    auto lean_share = [&](const ojson* r) -> std::pair<long, long> {
        auto m = share_re.search_match(ex(*r, "certificate_bitblast"));
        if (!m) return {0, 0};
        return {std::atol(m->group(1).c_str()), std::atol(m->group(2).c_str())};
    };
    long wrong_cert = 0;
    for (const auto& r : rows)
        if (r["stage"] == CERT_STAGE && !r["expected"].get<bool>() && st(r).count("PROVED-CERTIFIED")) ++wrong_cert;
    long lean = 0, z3 = 0, mixed = 0;
    for (const ojson* r : cert) {
        auto [a, b] = lean_share(r);
        if (b && a == b) ++lean;
        if (a == 0) ++z3;
        if (0 < a && a < b) ++mixed;
    }
    long looped_cert = 0;
    for (const ojson* r : looped)
        if (st(*r).count("PROVED-CERTIFIED")) ++looped_cert;
    long combined = 0, batched = 0, per_vc = 0;
    for (const ojson* r : all_cert) {
        std::string scope = ex(*r, "certificate_scope");
        if (scope == "combined") ++combined;
        else if (scope == "batched") ++batched;
        else ++per_vc;
    }
    ojson s = ojson::object();
    s["true_functions"] = static_cast<long>(sel.size());
    s["loop_free_true"] = static_cast<long>(loop_free.size());
    s["loop_free_true_proved"] = static_cast<long>(proved.size());
    // PROVED with no VC at all: nothing to certify (docs/TRUSTED_BASE.md)
    s["loop_free_true_no_vcs"] = static_cast<long>(no_vcs.size());
    s["loop_free_true_certified"] = static_cast<long>(cert.size());
    // which bit-blaster made the CNFs of the certified functions
    s["loop_free_true_certified_lean"] = lean;
    s["loop_free_true_certified_z3"] = z3;
    s["loop_free_true_certified_mixed"] = mixed;
    s["looped_true"] = static_cast<long>(looped.size());
    s["looped_true_certified"] = looped_cert;
    s["not_encoded_true"] = static_cast<long>(unencoded.size());
    s["wrong_certified"] = wrong_cert;
    s["proved_not_certified"] = not_cert;
    s["certified_combined"] = combined;
    s["certified_batched"] = batched;
    s["certified_per_vc"] = per_vc;
    return s;
}

namespace {
std::string join(const std::vector<std::string>& v, const std::string& sep) {
    std::string out;
    for (std::size_t i = 0; i < v.size(); ++i) {
        if (i) out += sep;
        out += v[i];
    }
    return out;
}

std::string rstrip(std::string s) {
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\n')) s.pop_back();
    return s;
}

std::string strip(std::string s) {
    s = rstrip(std::move(s));
    std::size_t a = 0;
    while (a < s.size() && (s[a] == ' ' || s[a] == '\t' || s[a] == '\n')) ++a;
    return s.substr(a);
}

std::string num(const ojson& j) { return jstr(j); }
}  // namespace

std::string markdown(const ojson& metrics, const std::vector<ojson>& rows, const ojson& meta) {
    std::vector<std::string> out = {"# PRISM conformance results", ""};
    std::vector<std::string> cmd, stg;
    for (const auto& c : meta["command"]) cmd.push_back(c.get<std::string>());
    for (const auto& s : meta["stages"]) stg.push_back(s.get<std::string>());
    const long wrong = meta["wrong_proofs"].get<long>();
    out.push_back("- engine: `" + meta["engine"].get<std::string>() + "` (`" + join(cmd, " ") + "`)");
    out.push_back("- stages run: `" + join(stg, ",") + "`");
    out.push_back("- tasks: " + num(meta["tasks"]) + " files, " + num(meta["functions"]) + " scored functions");
    out.push_back("- sandbox for counterexample replay: " + meta["sandbox"].get<std::string>());
    out.push_back(std::string("- **release gate: ") + (wrong == 0 ? "PASS" : "FAIL") + "** (" + std::to_string(wrong) +
                  " wrong proof(s))");
    out.push_back("");
    out.push_back(
        "| stage | origin | true | false | wrong proofs | completeness | detection (replayed cex) "
        "| refuted, not replayed | false alarms | BOUNDED | no answer | Law 6 ok |");
    out.push_back("|---|---|---|---|---|---|---|---|---|---|---|---|");
    for (const auto& [stage, by] : metrics.items()) {
        for (const auto& [origin, m] : by.items()) {
            if (m["functions"].get<long>() == 0) continue;
            long t = m["true"].get<long>(), f = m["false"].get<long>(), pr = m["proved"].get<long>(),
                 rr = m["refuted_replayed"].get<long>(), rf = m["refuted"].get<long>();
            out.push_back("| " + stage + " | " + origin + " | " + std::to_string(t) + " | " + std::to_string(f) +
                          " | **" + num(m["wrong_proofs"]) + "** | " + std::to_string(pr) + "/" + std::to_string(t) +
                          " (" + pct(pr, t) + ") | " + std::to_string(rr) + "/" + std::to_string(f) + " (" +
                          pct(rr, f) + ") | " + std::to_string(rf - rr) + " | " + num(m["false_alarms"]) + " | " +
                          num(m["bounded"]) + " | " + num(m["no_answer"]) + " | " + num(m["law_ok"]) + "/" +
                          num(m["law_tasks"]) + " |");
        }
    }
    out.push_back("");
    if (meta.contains("certified") && truthy(meta["certified"])) {
        const ojson& cs = meta["certified"];
        out.insert(out.end(),
                   {"## Certified mode (roadmap 3.2, `prism --certified`)", "",
                    "- loop-free `true` functions encoded by pir: " + num(cs["loop_free_true"]),
                    "- of those PROVED (any proof) under --certified: " + num(cs["loop_free_true_proved"]),
                    "- of those with no VC at all (PROVED, nothing to certify): " + num(cs["loop_free_true_no_vcs"]),
                    "- of those **PROVED-CERTIFIED**: **" + num(cs["loop_free_true_certified"]) + "/" +
                        num(cs["loop_free_true"]) + "** (CNF by the Lean-proved bit-blaster: " +
                        num(cs["loop_free_true_certified_lean"]) + ", by Z3's tactics: " +
                        num(cs["loop_free_true_certified_z3"]) + ", mixed: " +
                        num(cs["loop_free_true_certified_mixed"]) + ")",
                    "- `true` functions with loops: " + num(cs["looped_true"]) + " (" +
                        num(cs["looped_true_certified"]) + " PROVED-CERTIFIED: loops closed within the unwind)",
                    "- `true` functions pir did not encode (NEEDS-HARNESS etc.): " + num(cs["not_encoded_true"]),
                    "- PROVED-CERTIFIED on a `false` function (must be 0): **" + num(cs["wrong_certified"]) + "**",
                    "- PROVED-CERTIFIED functions (all): " + num(cs["certified_combined"]) +
                        " by one combined certificate, " + num(cs["certified_batched"]) + " by certified batches, " +
                        num(cs["certified_per_vc"]) + " by one certificate per VC"});
        if (cs.contains("run_seconds")) {
            std::vector<std::string> to;
            for (const auto& x : cs["timeouts"]) to.push_back(x.get<std::string>());
            out.push_back("- certified runs: " + num(cs["runs"]) + " tasks, " + num(cs["run_seconds"]) +
                          " s in total; " + std::to_string(to.size()) + " timed out" +
                          (to.empty() ? std::string() : " (" + join(to, ", ") + ")"));
        }
        out.push_back("");
        for (const auto& x : cs["proved_not_certified"])
            out.push_back("  - proved, not certified: `" + jstr(x["task"]) + "` `" + jstr(x["function"]) +
                          "`: " + jstr(x["note"]));
        out.push_back("");
    }
    // per-category matrix
    std::set<std::pair<std::string, std::string>> cats;
    for (const auto& r : rows) cats.insert({r["origin"].get<std::string>(), r["category"].get<std::string>()});
    std::vector<std::string> stages;
    for (const auto& [s, _] : metrics.items()) stages.push_back(s);
    if (!cats.empty()) {
        std::string head = "| origin/category | ";
        std::vector<std::string> cols;
        for (const auto& s : stages) cols.push_back(s + ": proved true | " + s + ": refuted false");
        head += join(cols, " | ") + " |";
        std::string rule = "|---|";
        for (std::size_t i = 0; i < stages.size(); ++i) rule += "---|---|";
        out.insert(out.end(), {"## Per category", "", head, rule});
        for (const auto& [origin, cat] : cats) {
            std::vector<std::string> cells;
            for (const auto& s : stages) {
                long tn = 0, fn = 0, pr = 0, rf = 0;
                for (const auto& r : rows) {
                    if (r["stage"] != s || r["origin"] != origin || r["category"] != cat || r["law_task"].get<bool>())
                        continue;
                    (r["expected"].get<bool>() ? tn : fn)++;
                    if (r["outcome"] == "proved") ++pr;
                    if (r["outcome"] == "refuted") ++rf;
                }
                cells.push_back(std::to_string(pr) + "/" + std::to_string(tn));
                cells.push_back(std::to_string(rf) + "/" + std::to_string(fn));
            }
            out.push_back("| " + origin + "/" + cat + " | " + join(cells, " | ") + " |");
        }
        out.push_back("");
    }

    auto listing = [&](const std::string& title, auto pred) {
        std::vector<const ojson*> sel;
        for (const auto& r : rows)
            if (pred(r)) sel.push_back(&r);
        out.push_back("## " + title + " (" + std::to_string(sel.size()) + ")");
        out.push_back("");
        if (sel.empty()) out.push_back("None.");
        for (const ojson* rp : sel) {
            const ojson& r = *rp;
            std::vector<std::string> descs, cexs;
            for (const auto& x : r["findings"]) {
                std::string cls = truthy(field(x, "cls")) ? jstr(field(x, "cls")) : "";
                std::string msg = truthy(field(x, "message")) ? jstr(field(x, "message")) : "";
                descs.push_back(strip(jstr(field(x, "status")) + " " + cls + " " + msg));
                if (truthy(field(x, "counterexample"))) cexs.push_back(jstr(field(x, "counterexample")));
            }
            std::string cex = join(cexs, "; ");
            out.push_back("- `" + jstr(r["task"]) + "` `" + jstr(r["function"]) + "` [" + jstr(r["stage"]) +
                          "]: " + join(descs, "; ") + (cex.empty() ? std::string() : " (cex: `" + cex + "`)"));
            if (r.contains("replay") && truthy(r["replay"])) {
                const ojson& rpl = r["replay"];
                const ojson& what = field(rpl, "replay");
                if (!what.is_null() && !(what.is_string() && what.get<std::string>() == "replayed")) {
                    auto get = [&](const char* k) { return rpl.contains(k) ? jstr(rpl[k]) : std::string(); };
                    out.push_back(rstrip("  - replay: " + jstr(what) + " " + get("outcome") + " " + get("why") + " " +
                                         get("detail")));
                }
            }
        }
        out.push_back("");
    };
    auto oc = [](const ojson& r) { return r["outcome"].get<std::string>(); };
    listing("Wrong proofs (soundness bugs)", [&](const ojson& r) { return oc(r) == "wrong-proof"; });
    listing("False alarms", [&](const ojson& r) { return oc(r) == "false-alarm"; });
    listing("Law 6 / expected-status violations",
            [&](const ojson& r) { return oc(r) == "law6-violation" || oc(r) == "unexpected-status"; });
    listing("Stage crashed on the task (every function of the file lost)", [&](const ojson& r) {
        for (const auto& f : r["findings"])
            if (status_of(f) == "STAGE-FAILED") return true;
        return false;
    });
    listing("Function never reported by the stage (silently skipped, Law 7)",
            [&](const ojson& r) { return oc(r) == "missing"; });
    listing("Refuted but counterexample did not replay", [&](const ojson& r) {
        if (oc(r) != "refuted" || r["origin"] != "prism") return false;
        return !(r.contains("replay") && r["replay"].value("replay", "") == "replayed");
    });
    return join(out, "\n") + "\n";
}

}  // namespace prism::qa
