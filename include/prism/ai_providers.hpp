#pragma once

// AI providers behind ai::ModelBackend. FORGE inference (in process, .fpk) is
// the default and needs nothing else; PRISM_AI selects any other AI:
//
//   PRISM_AI=forge      FORGE inference on PRISM_FPK (default)
//   PRISM_AI=openai     any OpenAI-compatible chat-completions server: OpenAI,
//                       vLLM, llama.cpp server, LM Studio, OpenRouter, Together, ...
//   PRISM_AI=ollama     Ollama's OpenAI-compatible endpoint (localhost:11434/v1)
//   PRISM_AI=anthropic  the Claude Messages API
//   PRISM_AI=cmd        any command: prompt on stdin, reply on stdout (PRISM_AI_CMD)
//
// PRISM_AI_URL (base URL), PRISM_AI_MODEL, PRISM_AI_KEY_ENV (the NAME of the
// environment variable that holds the API key; the key itself is never a PRISM
// variable, never on a command line and never in the audit log),
// PRISM_AI_GRAMMAR=1 (send the GBNF grammar to servers that accept one).
//
// A remote provider receives the prompts, which quote the code under analysis:
// it is used only when PRISM_AI names it. Whatever the provider, model output is
// HYPOTHESIS and is checked by PRISM's prover before it matters (Law 4).

#include "prism/ai.hpp"
#include "prism/export.hpp"

#include <memory>
#include <optional>
#include <string>

namespace prism::ai {

struct ProviderConfig {
    std::string kind = "forge";  // forge | openai | anthropic | cmd
    std::string url;             // base URL (openai: .../v1, anthropic: https://api.anthropic.com)
    std::string model;
    std::string key_env;         // name of the variable holding the key ("" = no key)
    std::string cmd;             // kind cmd: the command line (run by the shell)
    bool send_grammar = false;
    int max_tokens = 4096;
    double timeout_s = 180;
};

// From PRISM_AI* (aliases ollama/vllm/llamacpp/lmstudio -> openai with their
// local URL, claude -> anthropic). nullopt + *why for an unknown kind.
PRISM_API std::optional<ProviderConfig> provider_from_env(std::string* why);

// nullptr (and *why) for kind forge (the caller uses FORGE) or when the
// provider cannot be used (no curl, key variable unset, no command).
PRISM_API std::shared_ptr<ModelBackend> provider_backend(const ProviderConfig& pc, std::string* why);

// Exposed for tests: the request body and the reply text of one call.
PRISM_API std::string provider_request_json(const ProviderConfig& pc, const ModelRequest& req);
// The text of a provider's JSON answer; empty + *err on an error or refusal.
PRISM_API std::string provider_reply_text(const ProviderConfig& pc, const std::string& body, std::string* err);

}  // namespace prism::ai
