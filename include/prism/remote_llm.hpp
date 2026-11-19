#pragma once

// FORGE's remote chat client (forge::inference::remote: OpenAI-compatible
// servers such as Morph, OpenRouter, vLLM, forge-serve; the Anthropic
// Messages API; or any command) as a PRISM model backend. Configured only
// through FORGE's own variables (FORGE_AI_PROVIDER, FORGE_AI_URL,
// FORGE_AI_MODEL, FORGE_AI_KEY_ENV, ...; FORGE docs/inference.md "Remote
// models"); used when no .fpk is configured. Setting them is the opt-in:
// prompts (which quote code from the scanned tree, fenced as untrusted) go to
// that provider. The reply is a proposal like any model output (Law 4): every
// feature validates it and the checkers decide.

#include "prism/ai.hpp"
#include "prism/export.hpp"

#include <memory>
#include <string>

namespace prism::remote_llm {

// nullptr with *why == "" when no FORGE_AI_* setting is present; nullptr
// with a reason when the settings are wrong.
PRISM_API std::shared_ptr<ai::ModelBackend> from_env(std::string* why = nullptr);

}  // namespace prism::remote_llm
