// Stage llm: LLM hypotheses (HYPOTHESIS strength, never a proof).
#include "llm.hpp"

namespace prism {
namespace fs = std::filesystem;
using namespace stages_detail;

namespace {
const char* SYSTEM_AUDITOR =
    "You are PRISM, a code auditor. You READ code. You never claim a proof. "
    "Every defect you name is a HYPOTHESIS that must be checked by BMC, a "
    "fuzzer, or a human. Reply with JSON only: "
    "{\"hypotheses\":[{\"function\":\"...\",\"line\":0,\"cls\":\"INT-SIGNED-OVF\",\"why\":\"...\"}]}";

const char* SYSTEM_DAFNY =
    "Propose Dafny-style contracts for a C function. JSON: "
    "{\"requires\":[\"...\"],\"ensures\":[\"...\"],\"invariant\":[\"...\"],\"decreases\":[\"...\"]}. "
    "These are hypotheses.";

}  // namespace

std::vector<Finding> dafny_specs(const std::vector<FunctionInfo>& functions, int budget, const Config& cfg) {
    // Model-proposed contracts are HYPOTHESIS / READS (Law 4): they never
    // prove or cover FUNC-CONTRACT; a missing model is NOTRUN.
    LlamaEngine engine(cfg);
    if (!engine.available()) {
        Finding f;
        f.stage = "contracts";
        f.status = std::string(laws::NOTRUN);
        f.cls = "FUNC-CONTRACT";
        f.message = LLM_UNAVAILABLE_MSG;
        f.strength = std::string(laws::STRENGTH_READS);
        return {f};
    }
    std::vector<Finding> out;
    const int n = std::min(budget, static_cast<int>(functions.size()));
    for (int i = 0; i < n; ++i) {
        const auto& fn = functions[static_cast<std::size_t>(i)];
        auto r = engine.complete({{"system", SYSTEM_DAFNY}, {"user", fn.signature + "\n{" + fn.body + "\n}"}});
        if (!r.error.empty()) {
            if (llm_httpish(r.error)) {
                auto f = make_find("contracts", laws::NOTRUN, fn, "FUNC-CONTRACT", r.error, laws::STRENGTH_READS);
                f.extra["install"] = LLM_INSTALL;
                f.extra["backend"] = r.backend;
                out.push_back(std::move(f));
            } else {
                auto f = make_find("contracts", laws::ERROR, fn, "FUNC-CONTRACT", r.error, laws::STRENGTH_READS);
                f.extra["backend"] = r.backend;
                out.push_back(std::move(f));
            }
            continue;
        }
        auto f = make_find("contracts", laws::HYPOTHESIS, fn, "FUNC-CONTRACT",
                           "Dafny-style spec (hypothesis, not proved)", laws::STRENGTH_READS);
        f.extra["spec"] = extract_json(r.text).dump();
        f.extra["raw"] = r.text.substr(0, 800);
        f.extra["backend"] = r.backend;
        out.push_back(std::move(f));
    }
    return out;
}

std::vector<Finding> hypothesize(const std::vector<FunctionInfo>& functions, int budget, const Config& cfg) {
    // Missing GGUF/Ollama is NOTRUN, never CLEAN. Model text is HYPOTHESIS/READS.
    // Empty hypotheses are silence: still HYPOTHESIS, not a proof and not COVERED.
    LlamaEngine engine(cfg);
    if (!engine.available()) {
        Finding f;
        f.stage = "llm";
        f.status = std::string(laws::NOTRUN);
        f.cls = "INTENT";
        f.message = LLM_UNAVAILABLE_MSG;
        f.strength = std::string(laws::STRENGTH_READS);
        f.extra["install"] = LLM_INSTALL;
        f.extra["gguf"] = cfg.gguf.string();
#ifdef PRISM_HAS_LLAMA
        f.extra["prism_has_llama"] = "1";
#else
        f.extra["prism_has_llama"] = "0";
#endif
        return {f};
    }
    std::vector<Finding> out;
    int n = std::min(budget, static_cast<int>(functions.size()));
    for (int i = 0; i < n; ++i) {
        auto& fn = functions[static_cast<std::size_t>(i)];
        auto src = fn.signature + "\n{" + fn.body + "\n}";
        if (src.size() > 6000) src.resize(6000);
        auto r = engine.complete({{"system", SYSTEM_AUDITOR}, {"user", src}});
        if (!r.error.empty()) {
            // Python engine llm_complete_unavailable: HTTP/connection is a missing backend
            // (NOTRUN + LLM_INSTALL), never a code ERROR. Timed-out stays ERROR.
            if (llm_httpish(r.error)) {
                auto f = make_find("llm", laws::NOTRUN, fn, "INTENT", r.error, laws::STRENGTH_READS);
                f.extra["install"] = LLM_INSTALL;
                f.extra["backend"] = r.backend;
                out.push_back(std::move(f));
            } else {
                out.push_back(make_find("llm", laws::ERROR, fn, "INTENT", r.error, laws::STRENGTH_READS));
            }
            continue;
        }
        auto data = extract_json(r.text);
        if (!data.is_object() || !data.contains("hypotheses") || !data["hypotheses"].is_array() ||
            data["hypotheses"].empty()) {
            auto f = make_find("llm", laws::HYPOTHESIS, fn, "INTENT", r.text.substr(0, 400),
                               laws::STRENGTH_READS);
            f.extra["backend"] = r.backend;
            out.push_back(std::move(f));
            continue;
        }
        for (auto& h : data["hypotheses"]) {
            Finding f;
            f.stage = "llm";
            f.status = std::string(laws::HYPOTHESIS);
            f.file = fn.file;
            f.function = h.value("function", fn.name);
            if (h.contains("line") && h["line"].is_number_integer()) f.line = h["line"].get<int>();
            else f.line = fn.line;
            f.cls = h.value("cls", "INTENT");
            f.message = h.value("why", "");
            f.strength = std::string(laws::STRENGTH_READS);
            f.extra["backend"] = r.backend;
            out.push_back(std::move(f));
        }
    }
    return out;
}

}  // namespace prism
