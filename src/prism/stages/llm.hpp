#pragma once

// Internal to prism_core: the LLM chat engine (llama-server, Ollama or linked
// llama.cpp) and the sandboxed compile+run of LLM-written C
// (src/prism/stages/llm.cpp). Used by hypothesize, fuse, execute and repair.

#include "common.hpp"

#include <nlohmann/json.hpp>

namespace prism::stages_detail {

inline const char* LLM_UNAVAILABLE_MSG = "llama.cpp/Ollama not reachable";
inline const char* LLM_INSTALL = "ollama serve  (qwen3.5:9b) or build prism with -DPRISM_LLAMA=ON";

// Python engine llm_complete_unavailable: HTTP/connection is a missing backend.
bool llm_httpish(const std::string& err);

nlohmann::json extract_json(std::string text);

struct ChatResult {
    std::string text;
    std::string backend;
    std::string error;
};

struct LlamaEngine {
    Config cfg;
    std::string backend = "none";
    explicit LlamaEngine(Config c);
    void bind();
    bool available() const;
    // Roadmap 9.6: every model interaction goes to <out>/ai_audit.jsonl.
    // These chats never set a verdict by themselves (checker "none"); a caller
    // that checks the output (RLEF BMC) logs its own checked record.
    std::string last_prompt;
    ChatResult complete(const std::vector<std::pair<std::string, std::string>>& messages, double timeout = 180.0);
    ChatResult complete_raw(const std::vector<std::pair<std::string, std::string>>& messages, double timeout);
};

// Law 9: the program is LLM-written; it runs only with --allow-exec and then
// inside the sandbox (bubblewrap when available, rlimits always).
nlohmann::json sandbox_run(const std::string& source, double timeout = 8.0, bool allow_exec = false);
std::string sandbox_err(const nlohmann::json& result);
std::string c_from_llm(std::string src);

}  // namespace prism::stages_detail
