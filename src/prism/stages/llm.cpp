// The LLM chat engine (llama-server, Ollama or linked llama.cpp) and the
// sandboxed compile+run of LLM-written C (declared in llm.hpp).
#include "llm.hpp"

#ifdef PRISM_HAS_LLAMA
#  include "llama.h"
#endif

namespace prism {
namespace fs = std::filesystem;
using namespace stages_detail;

namespace stages_detail {
// Python engine llm_complete_unavailable: HTTP/connection is a missing backend.
bool llm_httpish(const std::string& err) {
    auto low = lower_copy(err);
    return low.find("http error") != std::string::npos || low.find("http ") != std::string::npos ||
           low.find("urlopen") != std::string::npos || low.find("urlerror") != std::string::npos ||
           low.find("connection refused") != std::string::npos ||
           low.find("connection reset") != std::string::npos ||
           low.find("connection aborted") != std::string::npos ||
           low.find("not reachable") != std::string::npos ||
           low.find("not loaded") != std::string::npos ||
           low.find("failed to connect") != std::string::npos ||
           low.find("name or service not known") != std::string::npos ||
           low.find("winerror") != std::string::npos || low.find("errno 111") != std::string::npos ||
           low.find("errno 104") != std::string::npos;
}

nlohmann::json extract_json(std::string text) {
    text = strip(text);
    if (text.starts_with("```")) {
        while (!text.empty() && text.front() == '`') text.erase(text.begin());
        auto nl = text.find('\n');
        if (nl != std::string::npos) text = text.substr(nl + 1);
        auto end = text.rfind("```");
        if (end != std::string::npos) text = text.substr(0, end);
    }
    try {
        return nlohmann::json::parse(text);
    } catch (...) {
        auto a = text.find('{');
        auto b = text.rfind('}');
        if (a != std::string::npos && b > a) {
            try {
                return nlohmann::json::parse(text.substr(a, b - a + 1));
            } catch (...) {
            }
        }
    }
    return nullptr;
}

}  // namespace stages_detail

namespace {
#ifdef PRISM_HAS_LLAMA
struct NativeLlama {
    std::mutex mu;
    llama_model* model = nullptr;
    std::string loaded_path;
    bool backends = false;

    ~NativeLlama() {
        if (model) llama_model_free(model);
    }

    llama_model* get(const fs::path& path, std::string& err) {
        std::lock_guard<std::mutex> lock(mu);
        if (!backends) {
            llama_log_set(
                [](enum ggml_log_level level, const char* text, void*) {
                    if (level >= GGML_LOG_LEVEL_ERROR) std::fputs(text, stderr);
                },
                nullptr);
            ggml_backend_load_all();
            llama_backend_init();
            backends = true;
        }
        auto s = path.string();
        if (model && loaded_path == s) return model;
        if (model) {
            llama_model_free(model);
            model = nullptr;
        }
        auto params = llama_model_default_params();
        params.n_gpu_layers = 0;
        model = llama_model_load_from_file(s.c_str(), params);
        if (!model) {
            err = "failed to load GGUF " + s;
            return nullptr;
        }
        loaded_path = std::move(s);
        return model;
    }
};

static NativeLlama g_native_llama;

static ChatResult native_llama_complete(const Config& cfg,
                                        const std::vector<std::pair<std::string, std::string>>& messages) {
    if (cfg.gguf.empty() || !fs::exists(cfg.gguf)) {
        return {"", "llama.cpp", "missing GGUF (set PRISM_GGUF)"};
    }
    std::string err;
    llama_model* model = g_native_llama.get(cfg.gguf, err);
    if (!model) return {"", "llama.cpp", err};

    const llama_vocab* vocab = llama_model_get_vocab(model);
    llama_context_params ctx_params = llama_context_default_params();
    ctx_params.n_ctx = 2048;
    ctx_params.n_batch = 2048;
    llama_context* ctx = llama_init_from_model(model, ctx_params);
    if (!ctx) return {"", "llama.cpp", "failed to create llama_context"};

    std::vector<llama_chat_message> chat;
    chat.reserve(messages.size());
    for (auto& [role, content] : messages) chat.push_back({role.c_str(), content.c_str()});
    const char* tmpl = llama_model_chat_template(model, nullptr);
    std::vector<char> formatted(llama_n_ctx(ctx));
    int n = llama_chat_apply_template(tmpl, chat.data(), chat.size(), true, formatted.data(),
                                      static_cast<int32_t>(formatted.size()));
    if (n > static_cast<int>(formatted.size())) {
        formatted.resize(static_cast<std::size_t>(n));
        n = llama_chat_apply_template(tmpl, chat.data(), chat.size(), true, formatted.data(),
                                      static_cast<int32_t>(formatted.size()));
    }
    std::string prompt;
    if (n < 0) {
        for (auto& [role, content] : messages) {
            prompt += role;
            prompt += ": ";
            prompt += content;
            prompt += "\n";
        }
        prompt += "assistant:";
    } else {
        prompt.assign(formatted.data(), static_cast<std::size_t>(n));
    }

    const int n_prompt = -llama_tokenize(vocab, prompt.c_str(), static_cast<int32_t>(prompt.size()), nullptr, 0,
                                         true, true);
    if (n_prompt <= 0) {
        llama_free(ctx);
        return {"", "llama.cpp", "failed to tokenize prompt"};
    }
    std::vector<llama_token> prompt_tokens(static_cast<std::size_t>(n_prompt));
    if (llama_tokenize(vocab, prompt.c_str(), static_cast<int32_t>(prompt.size()), prompt_tokens.data(),
                       static_cast<int32_t>(prompt_tokens.size()), true, true) < 0) {
        llama_free(ctx);
        return {"", "llama.cpp", "failed to tokenize prompt"};
    }

    llama_sampler* smpl = llama_sampler_chain_init(llama_sampler_chain_default_params());
    llama_sampler_chain_add(smpl, llama_sampler_init_temp(0.2f));
    llama_sampler_chain_add(smpl, llama_sampler_init_greedy());

    llama_batch batch = llama_batch_get_one(prompt_tokens.data(), static_cast<int32_t>(prompt_tokens.size()));
    std::string text;
    constexpr int kMaxNew = 256;
    for (int i = 0; i < kMaxNew; ++i) {
        if (llama_decode(ctx, batch) != 0) {
            llama_sampler_free(smpl);
            llama_free(ctx);
            return {"", "llama.cpp", "llama_decode failed"};
        }
        llama_token id = llama_sampler_sample(smpl, ctx, -1);
        if (llama_vocab_is_eog(vocab, id)) break;
        char buf[256];
        int n_piece = llama_token_to_piece(vocab, id, buf, sizeof(buf), 0, true);
        if (n_piece < 0) break;
        text.append(buf, static_cast<std::size_t>(n_piece));
        batch = llama_batch_get_one(&id, 1);
    }
    llama_sampler_free(smpl);
    llama_free(ctx);
    return {text, "llama.cpp", ""};
}
#endif

}  // namespace

namespace stages_detail {
LlamaEngine::LlamaEngine(Config c) : cfg(std::move(c)) { bind(); }
void LlamaEngine::bind() {
    if (http_ok(cfg.llama_server + "/health") || http_ok(cfg.llama_server + "/v1/models")) {
        backend = "llama-server";
        return;
    }
    if (!cfg.ollama_host.empty() && http_ok(cfg.ollama_host + "/api/tags")) {
        backend = "ollama-llamacpp";
        return;
    }
#ifdef PRISM_HAS_LLAMA
    if (!cfg.gguf.empty() && fs::exists(cfg.gguf)) {
        backend = "llama.cpp";
        return;
    }
#endif
    backend = "none";
}
bool LlamaEngine::available() const { return backend != "none"; }
// Roadmap 9.6: every model interaction goes to <out>/ai_audit.jsonl.
// These chats never set a verdict by themselves (checker "none"); a caller
// that checks the output (RLEF BMC) logs its own checked record.
ChatResult LlamaEngine::complete(const std::vector<std::pair<std::string, std::string>>& messages, double timeout) {
    auto r = complete_raw(messages, timeout);
    std::string prompt;
    for (auto& [role, content] : messages) prompt += role + ":\n" + content + "\n";
    ai::AuditRecord rec;
    rec.feature = "chat";
    rec.model = r.backend + ":" + cfg.model;
    rec.model_sha256 = "unknown";
    if (r.backend == "llama.cpp" && !cfg.gguf.empty()) {
        static std::map<std::string, std::string> cache;
        static std::mutex mu;
        std::lock_guard<std::mutex> lock(mu);
        auto key = cfg.gguf.string();
        if (!cache.count(key)) cache[key] = ai::sha256_file(cfg.gguf);
        if (!cache[key].empty()) rec.model_sha256 = cache[key];
    }
    rec.grammar = "none";
    rec.output_valid = r.error.empty();
    rec.rejected_reason = r.error;
    rec.checker = "none";
    rec.verdict_effect = "none";
    ai::audit_model_call(rec, prompt, r.text);
    last_prompt = prompt;
    return r;
}
ChatResult LlamaEngine::complete_raw(const std::vector<std::pair<std::string, std::string>>& messages, double timeout) {
    if (backend == "none")
        return {"", "none", "llama.cpp not loaded and Ollama not reachable"};
#ifdef PRISM_HAS_LLAMA
    if (backend == "llama.cpp") return native_llama_complete(cfg, messages);
#endif
    nlohmann::json msgs = nlohmann::json::array();
    for (auto& [role, content] : messages) msgs.push_back({{"role", role}, {"content", content}});
    int ms = static_cast<int>(timeout * 1000);
    if (backend == "llama-server") {
        nlohmann::json body{{"model", cfg.model}, {"messages", msgs}, {"temperature", 0.2}};
        auto resp = http_request("POST", cfg.llama_server + "/v1/chat/completions", body.dump(), ms);
        if (!resp) return {"", "llama-server", "HTTP error"};
        try {
            auto raw = nlohmann::json::parse(*resp);
            std::string text = raw.value("choices", nlohmann::json::array()).empty()
                                   ? ""
                                   : raw["choices"][0]["message"].value("content", "");
            return {text, "llama-server", ""};
        } catch (const std::exception& ex) {
            return {"", "llama-server", ex.what()};
        }
    }
    nlohmann::json body{{"model", cfg.model},
                        {"messages", msgs},
                        {"stream", false},
                        {"think", false},
                        {"options", {{"temperature", 0.2}, {"num_ctx", 8192}}}};
    auto resp = http_request("POST", cfg.ollama_host + "/api/chat", body.dump(), ms);
    if (!resp) return {"", "ollama-llamacpp", "HTTP error"};
    try {
        auto raw = nlohmann::json::parse(*resp);
        std::string text = raw.value("message", nlohmann::json::object()).value("content", "");
        return {text, "ollama-llamacpp", ""};
    } catch (const std::exception& ex) {
        return {"", "ollama-llamacpp", ex.what()};
    }
}

// Law 9: the program is LLM-written; it runs only with --allow-exec and then
// inside the sandbox (bubblewrap when available, rlimits always).
nlohmann::json sandbox_run(const std::string& source, double timeout, bool allow_exec) {
    if (!allow_exec)
        return {{"ok", false}, {"error", "exec-disabled"}, {"stdout", ""}, {"stderr", ""}, {"code", nullptr}};
    auto cc = which_cc();
    if (!cc) return {{"ok", false}, {"error", "no compiler"}, {"stdout", ""}, {"stderr", ""}, {"code", nullptr}};
    auto td = fs::temp_directory_path() / ("prism_oci_" + std::to_string(std::random_device{}()));
    fs::create_directories(td);
    struct Guard {
        fs::path p;
        ~Guard() {
            std::error_code ec;
            fs::remove_all(p, ec);
        }
    } guard{td};
    auto src = td / "prog.c";
    {
        std::ofstream out(src);
        out << source;
    }
#ifdef _WIN32
    auto exe = td / "prog.exe";
#else
    auto exe = td / "prog";
#endif
    auto cr = run_argv({cc->string(), "-std=c11", "-O0", "-Wall", src.string(), "-o", exe.string()}, {}, 30.0);
    if (cr.timeout)
        return {{"ok", false}, {"error", "compile-timeout"}, {"stdout", ""}, {"stderr", ""}, {"code", nullptr}};
    if (cr.rc != 0)
        return {{"ok", false},
                {"error", "compile"},
                {"stdout", cr.out.substr(0, 2000)},
                {"stderr", cr.err.substr(0, 2000)},
                {"code", cr.rc}};
    auto rr = run_argv(sandbox::wrap_argv({exe.string()}, td), {}, timeout, sandbox::limits_for(timeout));
    bool ok = rr.rc == 0 && !rr.timeout;
    bool crashed = rr.crashed || rr.rc < 0 || static_cast<unsigned>(rr.rc) >= 0xC0000000u ||
                   rr.err.find("Aborted") != std::string::npos;
    nlohmann::json err = nlohmann::json(nullptr);
    if (!ok) {
        if (rr.timeout) err = "timeout";
        else if (crashed) err = "crash";
        else err = "exit";
    }
    return {{"ok", ok},
            {"error", err},
            {"stdout", rr.out.substr(0, 2000)},
            {"stderr", rr.err.substr(0, 2000)},
            {"code", rr.timeout ? nlohmann::json(nullptr) : nlohmann::json(rr.rc)},
            {"sandbox", sandbox::kind()}};
}

std::string sandbox_err(const nlohmann::json& result) {
    if (!result.contains("error") || result["error"].is_null()) return {};
    if (result["error"].is_string()) return result["error"].get<std::string>();
    return {};
}

std::string c_from_llm(std::string src) {
    src = strip(src);
    if (src.empty()) return {};
    auto fence = src.find("```");
    if (fence != std::string::npos) {
        src = src.substr(fence + 3);
        auto nl = src.find('\n');
        if (nl != std::string::npos) src = src.substr(nl + 1);
        auto end = src.rfind("```");
        if (end != std::string::npos) src = src.substr(0, end);
    }
    return strip(src);
}

}  // namespace stages_detail

}  // namespace prism
