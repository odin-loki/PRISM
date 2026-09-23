// Stage repair: RLEF repair loop (LLM candidate, sandbox run, BMC reward).
#include "llm.hpp"

namespace prism {
namespace fs = std::filesystem;
using namespace stages_detail;

namespace {
const char* SYSTEM_REPAIR =
    "You receive C source plus compiler/sanitizer/BMC output. Reply with "
    "a full corrected C file only, no markdown fences unless the file itself "
    "needs them. Preserve function names.";

struct RlefScore {
    int score = 0;
    std::string proved;
};

RlefScore rlef_reward(const nlohmann::json& result, std::string_view bmc_status) {
    RlefScore s;
    auto err = sandbox_err(result);
    if (err != "compile" && err != "compile-timeout" && err != "no compiler") s.score += 1;
    if (result.value("ok", false)) s.score += 2;
    if (err == "crash") s.score -= 1;
    if (laws::is_proof(bmc_status)) {
        s.proved = std::string(bmc_status);
        return s;
    }
    if (bmc_status == laws::FAILED) s.score -= 2;
    return s;
}

std::string bmc_status_of_source(const std::string& source, int unwind = 2) {
    auto src = strip(source);
    if (src.empty()) return {};
    auto td = fs::temp_directory_path() / ("prism_rlef_bmc_" + std::to_string(std::random_device{}()));
    std::error_code ec;
    fs::create_directories(td, ec);
    auto p = td / "cand.c";
    {
        std::ofstream out(p);
        out << src;
    }
    auto fns = extract_functions(p, "cand.c");
    fs::remove_all(td, ec);
    const FunctionInfo* fn = nullptr;
    for (auto& f : fns)
        if (f.kind == "SCALAR") {
            fn = &f;
            break;
        }
    if (!fn) return {};
    auto recs = run_bmc({*fn}, unwind);
    if (recs.empty()) return {};
    auto st = recs[0].status;
    if (st == laws::NOTRUN || st == laws::ERROR || st == laws::TIMEOUT || st == laws::NEEDS_HARNESS ||
        st == laws::UNKNOWN || st == laws::CLEAN)
        return {};
    return st;
}

}  // namespace

std::vector<Finding> rlef_repair(const Finding& fail, const Config& cfg) {
    LlamaEngine engine(cfg);
    if (!engine.available()) {
        auto f = nr("repair", LLM_UNAVAILABLE_MSG);
        f.extra["install"] = LLM_INSTALL;
        return {f};
    }
    if (!which_cc()) {
        auto f = nr("repair", "gcc/clang not on PATH", std::string(laws::STRENGTH_FINDS));
        f.extra["install"] = "install gcc or clang";
        return {f};
    }
    if (!cfg.allow_exec) {
        auto f = sandbox::exec_notrun("repair", "repair (LLM-written candidates)");
        f.strength = std::string(laws::STRENGTH_READS);
        return {f};
    }
    fs::path src_path = fail.file;
    if (!fs::exists(src_path)) src_path = cfg.root / fail.file;
    std::string source;
    try {
        source = read_text_file(src_path);
    } catch (const std::exception& ex) {
        Finding f;
        f.stage = "repair";
        f.status = std::string(laws::ERROR);
        f.file = src_path.string();
        f.message = ex.what();
        f.strength = std::string(laws::STRENGTH_READS);
        return {f};
    }
    if (source.empty() && !fs::exists(src_path)) {
        Finding f;
        f.stage = "repair";
        f.status = std::string(laws::ERROR);
        f.file = src_path.string();
        f.message = "cannot read source";
        f.strength = std::string(laws::STRENGTH_READS);
        return {f};
    }
    std::string feedback = fail.stage + " " + fail.status + " " + fail.cls + " " + fail.message + " " +
                           fail.counterexample;
    int rounds = cfg.repair_rounds;
    int best_score = -1;
    std::string best_src = source;
    std::vector<std::pair<std::string, std::string>> messages{
        {"system", SYSTEM_REPAIR},
        {"user", "SOURCE:\n" + source + "\n\nFEEDBACK:\n" + feedback},
    };
    nlohmann::json history = nlohmann::json::array();
    for (int i = 0; i < rounds; ++i) {
        auto r = engine.complete(messages, 120);
        if (!r.error.empty()) {
            if (llm_httpish(r.error) && best_score < 0) {
                auto f = nr("repair", r.error);
                f.extra["install"] = LLM_INSTALL;
                f.extra["backend"] = r.backend;
                f.extra["history"] = history.dump();
                return {f};
            }
            history.push_back(r.error);
            break;
        }
        auto src = c_from_llm(r.text);
        if (src.empty()) {
            history.push_back({{"round", i + 1}, {"score", nlohmann::json(nullptr)}, {"error", "silent"}});
            messages.push_back({"assistant", r.text});
            messages.push_back({"user", "Reply with a complete corrected C file only."});
            continue;
        }
        auto result = sandbox_run(src, 8.0, /*allow_exec=*/true);
        auto err = sandbox_err(result);
        if (err == "no compiler") {
            auto f = nr("repair", "gcc/clang not on PATH", std::string(laws::STRENGTH_FINDS));
            f.extra["install"] = "install gcc or clang";
            f.extra["history"] = history.dump();
            return {f};
        }
        std::string bmc_st;
        if (err != "compile" && err != "compile-timeout") bmc_st = bmc_status_of_source(src, 2);
        auto scored = rlef_reward(result, bmc_st);
        history.push_back({{"round", i + 1},
                           {"score", scored.score},
                           {"error", err.empty() ? nlohmann::json(nullptr) : nlohmann::json(err)},
                           {"bmc", bmc_st.empty() ? nlohmann::json(nullptr) : nlohmann::json(bmc_st)}});
        if (scored.score > best_score) {
            best_score = scored.score;
            best_src = src;
        }
        if (!scored.proved.empty()) {
            // The proof comes from BMC on the candidate, never from the model:
            // log the checked record the finding points at (roadmap 9.6).
            ai::AuditRecord checked;
            checked.feature = "repair";
            checked.file = fail.file;
            checked.model = r.backend + ":" + cfg.model;
            checked.model_sha256 = "unknown";
            checked.grammar = "none";
            checked.output_valid = true;
            checked.checker = "bmc(rlef candidate)";
            checked.checker_result = scored.proved;
            // The proof is about the LLM-written patch, not the scanned code:
            // the finding stays HYPOTHESIS (Law 4), so no verdict changes.
            checked.verdict_effect = "none";
            ai::audit_model_call(checked, engine.last_prompt, r.text);
            Finding f;
            f.stage = "repair";
            f.status = std::string(laws::HYPOTHESIS);
            f.file = fail.file;
            f.function = fail.function;
            f.line = fail.line;
            f.cls = fail.cls;
            f.extra["ai_audit_id"] = checked.id;
            f.extra["ai_checker"] = checked.checker;
            f.extra["ai_checker_result"] = checked.checker_result;
            f.message = "verified fix: BMC " + scored.proved + " on the LLM-written patch, round " +
                        std::to_string(i + 1) +
                        " (terminal success; a claim about the patch, not the scanned code)";
            f.strength = std::string(laws::STRENGTH_READS);
            f.extra["history"] = history.dump();
            f.extra["best"] = best_src.substr(0, 1000);
            f.extra["bmc"] = scored.proved;
            f.extra["patch_verdict"] = scored.proved;
            f.extra["fix_label"] = "verified fix";
            return {f};
        }
        if (result.value("ok", false) && bmc_st != laws::FAILED) break;
        messages.push_back({"assistant", r.text});
        messages.push_back({"user", result.dump().substr(0, 1500)});
    }
    Finding f;
    f.stage = "repair";
    f.status = best_score >= 3 ? std::string(laws::CLEAN) : std::string(laws::HYPOTHESIS);
    f.message = "RLEF best score " + std::to_string(best_score) + " over " + std::to_string(history.size()) +
                " rounds (not a proof)";
    f.strength = std::string(laws::STRENGTH_READS);
    f.extra["history"] = history.dump();
    f.extra["best"] = best_src.substr(0, 1000);
    return {f};
}

}  // namespace prism
