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

namespace {
std::mutex g_chat_mu;
ChatBackend g_chat_backend;
ChatBackend chat_backend() {
    std::lock_guard<std::mutex> g(g_chat_mu);
    return g_chat_backend;
}
}  // namespace

void set_chat_backend_for_testing(ChatBackend backend) {
    std::lock_guard<std::mutex> g(g_chat_mu);
    g_chat_backend = std::move(backend);
}

}  // namespace stages_detail

namespace {
#ifdef PRISM_HAS_LLAMA
// Layers offloaded to the GPU: PRISM_N_GPU_LAYERS (default -1, all of them;
// a CPU-only build ignores it). On Windows a CUDA build needs MSVC's cl, so
// without it the model stays on the CPU instead of pretending CUDA works.
int native_gpu_layers() {
#ifdef _WIN32
    if (!Config{}.which({"cl"})) return 0;
#endif
    if (const char* e = std::getenv("PRISM_N_GPU_LAYERS"); e && *e) {
        try {
            return std::stoi(e);
        } catch (...) {
        }
    }
    return -1;
}

// Context of the linked model: prompt and reply together (the servers use 8192 too).
constexpr int kNativeCtx = 8192;

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
        params.n_gpu_layers = native_gpu_layers();
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
    std::error_code gec;
    if (cfg.gguf.empty() || !fs::is_regular_file(cfg.gguf, gec) || fs::file_size(cfg.gguf, gec) <= 1'000'000) {
        return {"", "llama.cpp", "missing GGUF (set PRISM_GGUF)"};
    }
    std::string err;
    llama_model* model = g_native_llama.get(cfg.gguf, err);
    if (!model) return {"", "llama.cpp", err};

    const llama_vocab* vocab = llama_model_get_vocab(model);
    llama_context_params ctx_params = llama_context_default_params();
    ctx_params.n_ctx = kNativeCtx;
    ctx_params.n_batch = kNativeCtx;
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
    // The reply may use the rest of the context (a repaired C file is long);
    // generation stops at end of generation or when the context is full.
    const int max_new = static_cast<int>(llama_n_ctx(ctx)) - n_prompt;
    if (max_new <= 0) {
        llama_sampler_free(smpl);
        llama_free(ctx);
        return {"", "llama.cpp", "prompt longer than the context (" + std::to_string(n_prompt) + " tokens)"};
    }
    for (int i = 0; i < max_new; ++i) {
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
    if (chat_backend()) {
        backend = "test-double";
        return;
    }
    if (http_ok(cfg.llama_server + "/health") || http_ok(cfg.llama_server + "/v1/models")) {
        backend = "llama-server";
        return;
    }
    if (!cfg.ollama_host.empty() && http_ok(cfg.ollama_host + "/api/tags")) {
        backend = "ollama-llamacpp";
        return;
    }
#ifdef PRISM_HAS_LLAMA
    // A real GGUF, not a placeholder: more than 1 MB.
    std::error_code ec;
    if (!cfg.gguf.empty() && fs::is_regular_file(cfg.gguf, ec) && fs::file_size(cfg.gguf, ec) > 1'000'000 && !ec) {
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
    if (backend == "test-double") {
        if (auto fn = chat_backend()) return fn(messages, timeout);
        return {"", "test-double", "test chat backend cleared"};
    }
#ifdef PRISM_HAS_LLAMA
    if (backend == "llama.cpp") return native_llama_complete(cfg, messages);
#endif
    nlohmann::json msgs = nlohmann::json::array();
    for (auto& [role, content] : messages) msgs.push_back({{"role", role}, {"content", content}});
    int ms = static_cast<int>(timeout * 1000);
    if (backend == "llama-server") {
        nlohmann::json body{{"model", cfg.model}, {"messages", msgs}, {"temperature", 0.2}};
        std::string herr;
        auto resp = http_request("POST", cfg.llama_server + "/v1/chat/completions", body.dump(), ms, &herr);
        if (!resp) return {"", "llama-server", herr.empty() ? "HTTP error" : herr};
        try {
            auto raw = nlohmann::json::parse(*resp);
            std::string text;
            if (raw.contains("choices") && raw["choices"].is_array() && !raw["choices"].empty() &&
                raw["choices"][0].is_object() && raw["choices"][0].contains("message") &&
                raw["choices"][0]["message"].is_object() && raw["choices"][0]["message"].contains("content") &&
                raw["choices"][0]["message"]["content"].is_string())
                text = raw["choices"][0]["message"]["content"].get<std::string>();
            return {text, "llama-server", "", raw};
        } catch (const std::exception& ex) {
            return {"", "llama-server", ex.what()};
        }
    }
    nlohmann::json body{{"model", cfg.model},
                        {"messages", msgs},
                        {"stream", false},
                        {"think", false},
                        {"options", {{"temperature", 0.2}, {"num_ctx", 8192}}}};
    std::string herr;
    auto host = cfg.ollama_host;
    while (!host.empty() && host.back() == '/') host.pop_back();
    auto resp = http_request("POST", host + "/api/chat", body.dump(), ms, &herr);
    if (!resp) return {"", "ollama-llamacpp", herr.empty() ? "HTTP error" : herr};
    try {
        auto raw = nlohmann::json::parse(*resp);
        std::string text;
        if (raw.contains("message") && raw["message"].is_object() && raw["message"].contains("content") &&
            raw["message"]["content"].is_string())
            text = raw["message"]["content"].get<std::string>();
        return {text, "ollama-llamacpp", "", raw};
    } catch (const std::exception& ex) {
        return {"", "ollama-llamacpp", ex.what()};
    }
}

// Law 9: the program is LLM-written; it runs only with --allow-exec and then
// inside the sandbox (bubblewrap when available, rlimits always).
std::string tail(const std::string& s, std::size_t n) {
    if (s.size() <= n) return s;
    std::size_t b = s.size() - n;
    while (b < s.size() && (static_cast<unsigned char>(s[b]) & 0xC0) == 0x80) ++b;  // no split character
    return s.substr(b);
}

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
                {"stdout", tail(cr.out, 2000)},
                {"stderr", tail(cr.err, 2000)},
                {"code", cr.rc}};
#ifdef _WIN32
    // A crashing child must not hang on a Windows error dialog (inherited by
    // the child): SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX.
    SetErrorMode(0x0001 | 0x0002 | 0x8000);
#endif
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
            {"stdout", tail(rr.out, 2000)},
            {"stderr", tail(rr.err, 2000)},
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
    auto after_newline = [](const std::string& t) -> std::string {
        auto nl = t.find('\n');
        return nl == std::string::npos ? std::string() : t.substr(nl + 1);
    };
    if (src.starts_with("```")) {
        // The whole reply is fenced: drop the fence line (a fence with no
        // newline holds no file) and everything from the last closing fence.
        std::size_t a = 0, b = src.size();
        while (a < b && src[a] == '`') ++a;
        while (b > a && src[b - 1] == '`') --b;
        auto body = after_newline(src.substr(a, b - a));
        if (auto end = body.rfind("```"); end != std::string::npos) body = body.substr(0, end);
        return strip(body);
    }
    if (auto fence = src.find("```"); fence != std::string::npos) {
        auto body = src.substr(fence + 3);
        if (body.find('\n') != std::string::npos) body = after_newline(body);
        if (auto end = body.rfind("```"); end != std::string::npos) body = body.substr(0, end);
        return strip(body);
    }
    return src;
}

}  // namespace stages_detail

}  // namespace prism
