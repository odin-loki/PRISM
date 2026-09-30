// report.json -> SV-COMP answer (`prism svcomp`). docs/SVCOMP.md has the
// mapping rules; include/prism/svcomp.hpp summarises them.
#include "internal.hpp"

#include <algorithm>
#include <map>
#include <set>

namespace prism::svcomp {

using namespace detail;

namespace {

const std::vector<std::string> VERDICT_STAGES = {"bmc", "pir"};
// never PROVED-ASSUMING / BOUNDED (Law 2)
const std::set<std::string> PROOF_TRUE = {"PROVED", "PROVED-UNBOUNDED", "PROVED-CERTIFIED"};

// property -> which stages' proofs of main cover it, which finding classes refute it
struct PropSpec {
    std::set<std::string> prove;
    std::set<std::string> classes;
    std::optional<std::set<std::string>> props;                  // only these checks refute
    std::map<std::string, std::set<std::string>> refute_props;  // class -> only these checks
};

const std::map<std::string, PropSpec>& properties() {
    static const std::map<std::string, PropSpec> k = [] {
        std::map<std::string, PropSpec> m;
        // A signed left shift whose result is not representable is an overflow
        // under the SV-COMP rules ("the resulting type of an operation is a
        // signed-integer type but the resulting value is not in the range ...",
        // C11 6.5.7p4); a negative or too large shift count, or a negative left
        // operand, is not. refute_props names the INT-SHIFT-UB checks that can
        // be such an overflow (pir: shift-base, which also covers a negative
        // base; bmc: shift31); the replay under -fsanitize=shift-base then has to
        // show "left shift of N by M places cannot be represented".
        m["no-overflow"] = PropSpec{{"bmc", "pir"},
                                    {"INT-SIGNED-OVF", "INT-SHIFT-UB"},
                                    std::nullopt,
                                    {{"INT-SHIFT-UB", {"shift-base", "shift31"}}}};
        // Both verdict stages encode a reach_error() / __VERIFIER_error() call as
        // a FUNC-CONTRACT property "reach_error" (pir translate.cpp, bmc
        // model_call) and a canonical __VERIFIER_assert as "assert"; a proof of
        // main by either covers unreach-call.
        m["unreach-call"] =
            PropSpec{{"bmc", "pir"}, {"FUNC-CONTRACT"}, std::set<std::string>{"reach_error", "assert"}, {}};
        // valid-memtrack (leaks) and valid-free are not encoded by any verdict
        // stage: a proof never covers the whole property, so no `true`.
        m["valid-memsafety"] = PropSpec{{}, {"MEM-OOB-READ", "MEM-OOB-WRITE", "PTR-NULL-DEREF"}, std::nullopt, {}};
        return m;
    }();
    return k;
}

json with_stage(const json& f, const std::string& st) {
    json out = f.is_object() ? f : json::object();
    out["stage"] = st;
    return out;
}

}  // namespace

std::vector<std::pair<std::string, std::vector<json>>> main_findings(const json& report) {
    std::vector<std::pair<std::string, std::vector<json>>> per;
    if (!has(report, "stages") || !report.at("stages").is_array()) return per;
    for (const auto& st : report.at("stages")) {
        const std::string name = get_str(st, "name");
        if (std::find(VERDICT_STAGES.begin(), VERDICT_STAGES.end(), name) == VERDICT_STAGES.end()) continue;
        std::vector<json> rows;
        if (has(st, "findings") && st.at("findings").is_array())
            for (const auto& f : st.at("findings"))
                if (has(f, "function") && f.at("function").is_string() && f.at("function").get<std::string>() == "main")
                    rows.push_back(f);
        if (get_str(st, "status") == "failed") {
            std::string detail = has(st, "detail") ? pystr(st.at("detail")) : "";
            rows.push_back(json{{"status", "STAGE-FAILED"}, {"cls", ""}, {"message", detail.substr(0, 300)}});
        }
        // a report names each stage once; a repeated name replaces the earlier rows
        auto it = std::find_if(per.begin(), per.end(), [&](const auto& p) { return p.first == name; });
        if (it != per.end()) it->second = std::move(rows);
        else per.emplace_back(name, std::move(rows));
    }
    return per;
}

std::string finding_prop(const json& f) {
    const json& extra = extra_of(f);
    if (has(extra, "prop") && truthy(extra.at("prop"))) return pystr(extra.at("prop"));
    std::string msg = get_str(f, "message");
    auto colon = msg.find(':');
    return colon == std::string::npos ? "" : strip(std::string_view(msg).substr(0, colon));
}

bool refutes(const json& f, const std::string& prop) {
    const auto& spec = properties().at(prop);
    if (get_str(f, "status") != "FAILED") return false;
    if (!has(f, "cls") || !f.at("cls").is_string()) return false;
    const std::string cls = f.at("cls").get<std::string>();
    if (!spec.classes.contains(cls)) return false;
    if (spec.props) return spec.props->contains(finding_prop(f));
    auto only = spec.refute_props.find(cls);
    if (only != spec.refute_props.end()) return only->second.contains(finding_prop(f));
    return true;
}

Decision decide(const json& report, const std::string& prop, const std::function<json(const json&)>& replay_fn) {
    if (!properties().contains(prop)) return Decision{"unknown", "property " + prop + " is not supported", std::nullopt, json::object()};
    auto per = main_findings(report);
    bool any = std::any_of(per.begin(), per.end(), [](const auto& p) { return !p.second.empty(); });
    if (!any) return Decision{"unknown", "no verdict stage reported main", std::nullopt, json::object()};
    std::vector<std::pair<std::string, json>> refutations, proofs;
    for (const auto& [st, rows] : per)
        for (const auto& f : rows)
            if (refutes(f, prop)) refutations.emplace_back(st, f);
    // Replay first a refutation that also gives the nondet calls' source
    // positions (its witness can place every function_return waypoint), pir's
    // before bmc's (pir's trace stops at the violated check itself).
    std::stable_sort(refutations.begin(), refutations.end(), [](const auto& a, const auto& b) {
        auto key = [](const auto& sf) {
            return std::pair<bool, bool>{!has(extra_of(sf.second), "nondet_loc"), sf.first != "pir"};
        };
        return key(a) < key(b);
    });
    const auto& prove = properties().at(prop).prove;
    for (const auto& [st, rows] : per)
        for (const auto& f : rows)
            if (PROOF_TRUE.contains(get_str(f, "status")) && prove.contains(st)) proofs.emplace_back(st, f);
    json last_replay = json::object();
    for (const auto& [st, f] : refutations) {
        json rp = replay_fn(f);
        if (!rp.is_object()) rp = json::object();
        last_replay = rp;
        if (get_str(rp, "replay") == "replayed") {
            std::string sub = has(rp, "subproperty") && truthy(rp.at("subproperty")) ? pystr(rp.at("subproperty")) : prop;
            std::string why = st + ": FAILED " + get_str(f, "cls", "None") + " (" + get_str(f, "message") +
                              "); counterexample replayed: " + get_str(rp, "detail");
            if (!proofs.empty()) {
                why += "; DISAGREEMENT: ";
                for (std::size_t i = 0; i < proofs.size(); ++i) why += (i ? ", " : "") + proofs[i].first;
                why += " claimed a proof";
            }
            return Decision{"false(" + sub + ")", why, with_stage(f, st), rp};
        }
    }
    if (!refutations.empty()) {
        const auto& [st, f] = refutations.front();
        std::string why_nr = has(last_replay, "why") ? get_str(last_replay, "why") : get_str(last_replay, "detail");
        return Decision{"unknown",
                        st + ": FAILED " + get_str(f, "cls", "None") + " but the counterexample did not replay (" +
                            get_str(last_replay, "replay", "None") + ": " + why_nr + ")",
                        with_stage(f, st), last_replay};
    }
    if (!proofs.empty()) {
        const auto& [st, f] = proofs.front();
        return Decision{"true", st + ": " + get_str(f, "status", "None") + " of main (" + get_str(f, "message") + ")",
                        with_stage(f, st), json::object()};
    }
    std::string summary;
    for (std::size_t i = 0; i < per.size(); ++i) {
        std::set<std::string> sts;
        for (const auto& f : per[i].second) sts.insert(get_str(f, "status", "None"));
        std::string joined;
        for (const auto& s : sts) joined += (joined.empty() ? "" : ", ") + s;
        summary += (i ? "; " : "") + per[i].first + ": " + (joined.empty() ? "none" : joined);
    }
    return Decision{"unknown", "no proof covering " + prop + " and no replayed refutation (" + summary + ")", std::nullopt, json::object()};
}

}  // namespace prism::svcomp
