#pragma once

// Internal to prism_core: the pieces of the execute and repair stages that
// tests drive directly (src/prism/stages/execute_cex.cpp, rlef.cpp). None of
// them can produce a proof about the scanned code: a sandbox CLEAN is not a
// proof, and a BMC proof of an LLM-written patch stays a claim about the patch.

#include "llm.hpp"

#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

namespace prism::stages_detail {

// sandbox_run result -> status: NOTRUN (no compiler / exec disabled), CLEAN,
// CRASH or FAILED. Never PROVED.
std::string sandbox_verdict(const nlohmann::json& result);

// RLEF reward: compile + run + sanitizer + BMC. BMC FAILED is negative; a
// BMC proof is terminal (`proved`); CLEAN and BOUNDED are never a proof.
struct RlefScore {
    int score = 0;
    std::string proved;
};
RlefScore rlef_reward(const nlohmann::json& result, std::string_view bmc_status);

// The OpenCodeInterpreter loop: the model writes a C main(), the sandbox runs
// it (only with allow_exec, Law 9).
std::vector<Finding> interpreter_loop(LlmEngine& engine, const std::string& prompt, int rounds, bool allow_exec);

// Status of the first SCALAR function of a candidate at a small unwind, or ""
// when BMC gives no answer (NOTRUN, ERROR, TIMEOUT, NEEDS-HARNESS, UNKNOWN).
using BmcOracle = std::function<std::string(const std::string& source)>;
std::string bmc_status_of_source(const std::string& source, int unwind = 2);

// rlef_repair with the BMC oracle replaceable (tests); empty = bmc_status_of_source.
std::vector<Finding> rlef_repair_with(const Finding& fail, const Config& cfg, const BmcOracle& oracle);

// Fuzz4All / ChatFuzz reply parsing (src/prism/stages/fuse.cpp).
// A hex blob as the reference engine reads it: every "0x"/"0X" dropped,
// whitespace between bytes allowed, never inside one; nullopt when invalid.
std::optional<std::vector<uint8_t>> parse_hex_bytes(std::string h);
// data[key] as a list of non-empty hex blobs (a bare number is read as its digits).
std::vector<std::vector<uint8_t>> json_hex_list(const nlohmann::json& data, const char* key);
// prism::score_prompt_seeds (Fuzz4All validate_prompt: the number of distinct
// encodings, padded / cut to nbytes) and prism::documentation_from_comments
// (Fuzz4All's path_documentation: up to 8 comments, contract comments
// (requires/ensures/invariant/decreases/diff:) and ACSL /*@ blocks left out)
// are public (prism/stages.hpp); named here so tests reach every piece as sd::.
using prism::documentation_from_comments;
using prism::score_prompt_seeds;

}  // namespace prism::stages_detail
