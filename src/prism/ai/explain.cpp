// Counterexample explanation + verified repair (roadmap 4.2, 9.3).
//
// The explanation is HYPOTHESIS with strength READS: it never changes the
// FAILED verdict it explains. A proposed fix is re-verified by running BMC on
// the patched function; it is labelled "verified fix" only when that run is a
// proof, otherwise "unverified suggestion". Nothing is compiled or executed.

#include "ai_internal.hpp"

#include "prism/cparse.hpp"
#include "prism/laws.hpp"
#include "prism/stages.hpp"

#include <nlohmann/json.hpp>

namespace prism::ai {
namespace fs = std::filesystem;

namespace {

Finding row(const Finding& fail, std::string_view status, std::string msg) {
    Finding f;
    f.stage = "repair";
    f.status = std::string(status);
    f.file = fail.file;
    f.function = fail.function;
    f.line = fail.line;
    f.cls = fail.cls;
    f.message = std::move(msg);
    f.strength = std::string(laws::STRENGTH_READS);
    f.extra["explains"] = fail.stage + ":" + fail.status + ":" + fail.cls;
    return f;
}

std::optional<FunctionInfo> find_function(const Finding& fail, const Config& cfg) {
    if (!fail.function) return std::nullopt;
    std::vector<fs::path> cands{fs::path(fail.file), cfg.root / fail.file};
    for (auto& p : cands) {
        std::error_code ec;
        if (!fs::is_regular_file(p, ec)) continue;
        for (auto& fn : extract_functions(p, fail.file))
            if (fn.name == *fail.function) return fn;
    }
    return std::nullopt;
}

}  // namespace

std::vector<Finding> explain_failed(const Finding& fail, const Config& cfg) {
    std::string why;
    std::shared_ptr<ModelBackend> backend =
        session_config() ? session_backend(&why) : connect_backend(cfg, &why);
    if (!backend) {
        auto f = row(fail, laws::NOTRUN, "counterexample explanation / verified repair: " + why);
        f.extra["feature"] = "explain";
        return {f};
    }
    auto fn = find_function(fail, cfg);
    if (!fn) {
        auto f = row(fail, laws::NOTRUN, "counterexample explanation: function source not found");
        f.extra["feature"] = "explain";
        return {f};
    }
    ModelRequest req;
    req.feature = "explain";
    req.grammar = "explain";
    req.grammar_text = grammar_text("explain");
    req.system = system_prompt(
        "Task: explain in two or three plain sentences why the counterexample triggers the reported defect, "
        "and propose a corrected body for the same function (statements between the braces only, same "
        "signature), or an empty string when unsure. Reply with the JSON object only.");
    req.user = "Finding: " + fail.stage + " " + fail.status + " " + fail.cls + ": " + fail.message +
               "\nCounterexample (from the prover, trusted): " + fail.counterexample + "\n" +
               fence_untrusted(function_source(*fn), "SOURCE");
    AuditRecord ar;
    ar.function = fn->name;
    ar.file = fn->file;
    auto reply = ask(*backend, req, ar);
    if (!reply.error.empty()) {
        ar.checker = "none";
        audit_append(ar);
        auto f = row(fail, laws::NOTRUN, "counterexample explanation: " + reply.error);
        f.extra["feature"] = "explain";
        f.extra["ai_audit_id"] = ar.id;
        return {f};
    }
    auto v = validate_explain(reply.text);
    if (!v.ok) {
        ar.rejected_reason = v.reason;
        ar.checker = "grammar-validator";
        ar.checker_result = "rejected";
        audit_append(ar);
        auto f = row(fail, laws::ERROR, "counterexample explanation rejected: " + v.reason);
        f.extra["feature"] = "explain";
        f.extra["ai_audit_id"] = ar.id;
        return {f};
    }
    ar.output_valid = true;
    std::vector<Finding> out;
    auto ex = row(fail, laws::HYPOTHESIS, "explanation (model, not a verdict): " + v.explanation.substr(0, 300));
    ex.extra["feature"] = "explain";
    ex.extra["explanation"] = v.explanation;
    ex.extra["model"] = backend->name();
    ex.extra["ai_audit_id"] = ar.id;
    out.push_back(std::move(ex));
    if (trim(v.fix_body).empty()) {
        ar.checker = "none";
        ar.checker_result = "";
        audit_append(ar);
        return out;
    }
    std::string label, fix_status;
    if (fn->kind == "POINTER" || fn->kind == "OTHER") {
        fix_status = std::string(laws::NEEDS_HARNESS);
        label = "unverified suggestion";
    } else {
        auto patched = *fn;
        patched.body = v.fix_body;
        auto unwind = session_config() ? session_config()->unwind : cfg.unwind;
        auto recs = run_bmc({patched}, unwind);
        fix_status = recs.empty() ? std::string(laws::ERROR) : recs[0].status;
        bool proved = fix_status == laws::PROVED || fix_status == laws::PROVED_UNBOUNDED;
        label = proved ? "verified fix" : "unverified suggestion";
    }
    ar.checker = "bmc(patched function)";
    ar.checker_result = fix_status;
    ar.verdict_effect = "none";  // a fix never changes the FAILED verdict it answers
    audit_append(ar);
    auto fx = row(fail, laws::HYPOTHESIS,
                  label + (label == "verified fix"
                               ? " (patched function " + fix_status +
                                     " by BMC: encoded UB properties only; intent/equivalence not checked)"
                               : " (patched function " + fix_status + " by BMC)"));
    fx.extra["feature"] = "repair";
    fx.extra["fix_label"] = label;
    fx.extra["fix_bmc_status"] = fix_status;
    fx.extra["fix_body"] = v.fix_body.substr(0, 2000);
    fx.extra["model"] = backend->name();
    fx.extra["ai_audit_id"] = ar.id;
    fx.extra["ai_checker"] = ar.checker;
    fx.extra["ai_checker_result"] = ar.checker_result;
    out.push_back(std::move(fx));
    return out;
}

}  // namespace prism::ai
