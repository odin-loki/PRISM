#pragma once
// PRISM's only language-model engine: FORGE inference (odin-loki/FORGE), in process,
// on a .fpk model (default Goedel-Prover-V2-32B). Replaces llama.cpp, llama-server and Ollama.

#include "prism/ai.hpp"

#include <filesystem>
#include <optional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace prism::forge_llm {

using Messages = std::vector<std::pair<std::string, std::string>>;  // (role, content)

struct Request {
    Messages messages;
    std::string grammar;       // GBNF (llama.cpp dialect, FORGE inference grammar); empty = free text
    int max_new = 768;
    float temperature = 0.2f;  // 0 = greedy
    unsigned seed = 0;
    double timeout_s = 180.0;  // decode stops (with error "timeout") once exceeded
    bool chat = true;          // false: messages are concatenated as a raw completion prompt
    // Thinking (chat only). false: the assistant turn opens with an empty <think></think>
    // block, so the model answers at once. true: the model reasons freely (no grammar)
    // until it closes </think>, or until `think_max` tokens, when PRISM closes the block
    // itself; then the answer is decoded under `grammar`. The reasoning is returned in
    // Reply::thinking and is never parsed as the answer.
    bool think = false;
    int think_max = 4096;
};

struct Reply {
    std::string text;
    std::string error;  // empty on success
    std::string thinking;  // the <think> text when Request::think (empty otherwise)
    std::size_t prompt_tokens = 0, new_tokens = 0, thinking_tokens = 0;
    bool thinking_capped = false;  // PRISM closed </think> at think_max
    double load_s = 0, prefill_s = 0, decode_s = 0;  // wall time: model open (first use), prompt, generation
};

// True when `path` is an openable FORGE .fpk (magic checked, not loaded).
bool is_fpk(const std::filesystem::path& path);

// The Goedel-Prover-V2 chat template (ChatML: "<|im_start|>role\n...<|im_end|>\n" + the
// assistant opener). think=false closes an empty <think></think> block, so the model
// answers without reasoning; think=true leaves "<think>\n" open for the model to reason.
std::string render_chat(const Messages& messages, bool think = false);

// PRISM's grammars/*.gbnf in the dialect FORGE's grammar engine reads: bounded
// repetition X{m}, X{m,} and X{m,n} (which FORGE does not parse) is expanded into m
// copies plus nested optionals (n-m <= 16) or X*. complete() applies it to
// Request::grammar; the validators re-check the decoded text, bounds included.
std::string lower_grammar(const std::string& gbnf);

// Chat completion against the model at `fpk`. The model is opened once per path and
// shared (FORGE serialises runs on a model); each call gets a fresh session.
Reply complete(const std::filesystem::path& fpk, const Request& req);

// BLAKE3/SHA identity of the model file for the AI audit log (cached per path).
std::string model_digest(const std::filesystem::path& fpk);

// Device in use ("cpu", "cuda", "vulkan", ...): $PRISM_DEVICE, else $FORGE_DEVICE, else FORGE's choice.
std::string device();

// Mean-pooled embedding from a one-shot FORGE embedding graph (.fpk whose session kind is
// one-shot; input "input_ids", first output [seq, dim]). nullopt + *err on failure.
std::optional<std::vector<double>> embed(const std::filesystem::path& fpk, const std::string& text, std::string* err);

// ai::ModelBackend over a FORGE .fpk: used for the assistant model (PRISM_FPK) and the
// Lean prover model (PRISM_PROVER_FPK).
class Backend final : public ai::ModelBackend {
public:
    // think/think_max: see Request. The assistant backend answers without thinking; the
    // Lean prover backend (proof_search.cpp) sets its own mode explicitly.
    Backend(std::filesystem::path fpk, std::string model, float temperature = 0.2f, int max_new = 768,
            bool think = false, int think_max = 4096)
        : fpk_(std::move(fpk)), model_(std::move(model)), temperature_(temperature), max_new_(max_new),
          think_(think), think_max_(think_max) {}
    std::string name() const override { return "forge:" + model_; }
    std::string model_sha256() const override {
        auto d = model_digest(fpk_);
        return d.empty() ? "unknown" : d;
    }
    ai::ModelReply complete(const ai::ModelRequest& req) override {
        Request fr;
        fr.messages = {{"system", req.system}, {"user", req.user}};
        fr.grammar = req.grammar_text;
        fr.temperature = temperature_;
        fr.max_new = max_new_;
        fr.think = think_;
        fr.think_max = think_max_;
        auto r = forge_llm::complete(fpk_, fr);
        return {r.text, r.error};
    }

private:
    std::filesystem::path fpk_;
    std::string model_;
    float temperature_;
    int max_new_;
    bool think_;
    int think_max_;
};

}  // namespace prism::forge_llm
