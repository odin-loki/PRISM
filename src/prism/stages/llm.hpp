#pragma once

// Internal to prism_core: the LLM chat engine (FORGE mind on a .fpk model) and the sandboxed compile+run of LLM-written C
// (src/prism/stages/llm.cpp). Used by hypothesize, fuse, execute and repair.

#include "common.hpp"

#include <nlohmann/json.hpp>

#include <functional>

namespace prism::stages_detail {

inline const char* LLM_UNAVAILABLE_MSG = "no FORGE model (.fpk) configured";
inline const char* LLM_INSTALL = "convert Qwen 3.5 9B with FORGE (forge convert) and set PRISM_FPK=/path/qwen3.5-9b.fpk";

// Python engine llm_complete_unavailable: HTTP/connection is a missing backend.
bool llm_httpish(const std::string& err);

nlohmann::json extract_json(std::string text);

struct ChatResult {
    std::string text;
    std::string backend;
    std::string error;
    nlohmann::json raw = nlohmann::json::object();  // the backend's JSON answer, when there is one
};

// Test seam: while set, every LlmEngine binds to this chat function
// (backend name "test-double") instead of a server or a .fpk. nullptr clears it.
using ChatBackend =
    std::function<ChatResult(const std::vector<std::pair<std::string, std::string>>& messages, double timeout)>;
void set_chat_backend_for_testing(ChatBackend backend);

struct LlmEngine {
    Config cfg;
    std::string backend = "none";
    explicit LlmEngine(Config c);
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
// Output is kept as its last 2000 bytes (where a crash report is), also when
// the run times out.
nlohmann::json sandbox_run(const std::string& source, double timeout = 8.0, bool allow_exec = false);
// The last n bytes of s (whole UTF-8 characters).
std::string tail(const std::string& s, std::size_t n);
std::string sandbox_err(const nlohmann::json& result);
// The C file of an LLM reply: a reply that opens with a fence is the fenced
// block (nothing when the fence has no newline); otherwise the first fenced
// block when there is one, else the whole reply.
std::string c_from_llm(std::string src);

}  // namespace prism::stages_detail
