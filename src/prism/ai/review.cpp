// The review stage (roadmap 9.2 proof repair, 9.3 assumption auditing and
// contracts from requirements, 4.2 contract drafting). It runs after the
// solver stages (contracts, wp, bmc, pir, harness) and produces what a human
// reviewer must look at:
//   1. VACUOUS-ASSUMPTION: requires / drafted ranges that no input satisfies (Z3);
//   2. approved contracts proved (PROVED-ASSUMING) and callers checked;
//      drafted contracts as HYPOTHESIS with trace links (model);
//   3. model flags on assumptions that may exclude realistic inputs (READS);
//   4. PROOF-REGRESSION against the proof store, with re-checked repairs.
// Without a model the deterministic parts run and one NOTRUN row names the
// model features that did not.

#include "ai_internal.hpp"

#include "prism/ai_proof.hpp"
#include "prism/laws.hpp"

namespace prism::ai {

std::vector<Finding> run_review(const std::vector<FunctionInfo>& functions, const std::vector<StageResult>& stages,
                                const Config& cfg) {
    std::vector<Finding> out;
    std::vector<Finding> audited;
    for (auto& s : stages)
        if (s.name == "contracts" || s.name == "wp" || s.name == "harness")
            audited.insert(audited.end(), s.findings.begin(), s.findings.end());

    auto contracts = draft_contracts(functions, cfg);
    std::vector<Finding> with_drafts = audited;
    for (auto& f : contracts)
        if (f.extra.count("contract_state") && f.extra.at("contract_state") == "approved") with_drafts.push_back(f);

    auto vac = vacuity_audit(functions, with_drafts, "review");
    out.insert(out.end(), vac.begin(), vac.end());
    out.insert(out.end(), contracts.begin(), contracts.end());

    auto flags = model_assumption_audit(functions, with_drafts, "review");
    out.insert(out.end(), flags.begin(), flags.end());

    std::vector<StageResult> so_far = stages;
    StageResult self;
    self.name = "review";
    self.findings = out;
    so_far.push_back(self);
    auto reg = run_proof_regression(functions, so_far, cfg);
    out.insert(out.end(), reg.begin(), reg.end());

    std::string why;
    if (!session_backend(&why)) {
        Finding f;
        f.stage = "review";
        f.status = std::string(laws::NOTRUN);
        f.message = "model features of review (contract drafting from code/comments/requirements, assumption "
                    "audit, proof repair) not run: " + why + "; the Z3 vacuity audit, approved contracts and the "
                    "proof-regression check ran";
        f.strength = std::string(laws::STRENGTH_READS);
        f.extra["install"] = "start llama-server (PRISM_LLAMA_SERVER) or Ollama with a local model";
        out.push_back(std::move(f));
    }
    return out;
}

}  // namespace prism::ai
