// AI providers: FORGE by default, any other AI through PRISM_AI
// (include/prism/ai_providers.hpp). HTTP goes through curl as a child process
// (prism::exec on FORGE's process layer): the request body is a private temp
// file and the API key reaches curl on stdin (`-K -`), never on a command line.

#include "prism/ai_providers.hpp"

#include "prism/exec.hpp"

#include <nlohmann/json.hpp>

#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>

namespace prism::ai {

namespace fs = std::filesystem;
using nlohmann::json;

namespace {

std::string env(const char* name) {
    const char* v = std::getenv(name);
    return v ? std::string(v) : std::string();
}

std::string curl_quote(const std::string& s) {
    std::string o = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') o += '\\';
        if (c == '\n' || c == '\r') continue;  // a header value never spans lines
        o += c;
    }
    return o + "\"";
}

std::string trim_slash(std::string u) {
    while (!u.empty() && u.back() == '/') u.pop_back();
    return u;
}

class HttpBackend final : public ModelBackend {
public:
    explicit HttpBackend(ProviderConfig pc) : pc_(std::move(pc)) {}
    std::string name() const override { return pc_.kind + ":" + pc_.model; }
    std::string model_sha256() const override { return "remote"; }
    ModelReply complete(const ModelRequest& req) override {
        const std::string body = provider_request_json(pc_, req);
        // private temp file for the body (prompts quote the analysed code)
        static std::atomic<unsigned> seq{0};
        const fs::path tmp = fs::temp_directory_path() /
                             ("prism-ai-" + std::to_string(std::random_device{}()) + "-" + std::to_string(++seq) + ".json");
        {
            std::ofstream o(tmp, std::ios::binary);
            o.close();
            std::error_code ec;
            fs::permissions(tmp, fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::replace, ec);
            o.open(tmp, std::ios::binary | std::ios::trunc);
            o << body;
        }
        struct Rm {
            fs::path p;
            ~Rm() {
                std::error_code ec;
                fs::remove(p, ec);
            }
        } rm{tmp};
        std::string cfg;
        std::string url = trim_slash(pc_.url);
        const std::string key = pc_.key_env.empty() ? std::string() : env(pc_.key_env.c_str());
        if (pc_.kind == "anthropic") {
            url += "/v1/messages";
            cfg += "header = " + curl_quote("x-api-key: " + key) + "\n";
            cfg += "header = " + curl_quote("anthropic-version: 2026-10-21") + "\n";
        } else {
            url += "/chat/completions";
            if (!key.empty()) cfg += "header = " + curl_quote("Authorization: Bearer " + key) + "\n";
        }
        cfg += "url = " + curl_quote(url) + "\n";
        cfg += "header = " + curl_quote("Content-Type: application/json") + "\n";
        cfg += "data-binary = " + curl_quote("@" + tmp.string()) + "\n";
        exec::SessionOptions so;
        so.timeout_s = pc_.timeout_s + 5;
        so.input = cfg;
        auto r = exec::run_session({"curl", "-sS", "--max-time", std::to_string(static_cast<int>(pc_.timeout_s)),
                                    "-K", "-"},
                                   so);
        if (r.failed || r.rc == 127) return {"", "curl not found (needed for PRISM_AI=" + pc_.kind + ")"};
        if (r.timed_out) return {"", "timeout"};
        if (r.rc != 0) return {"", "curl: " + (r.err.empty() ? "exit " + std::to_string(r.rc) : r.err.substr(0, 300))};
        std::string err;
        auto text = provider_reply_text(pc_, r.out, &err);
        return {text, err};
    }

private:
    ProviderConfig pc_;
};

class CmdBackend final : public ModelBackend {
public:
    explicit CmdBackend(ProviderConfig pc) : pc_(std::move(pc)) {}
    std::string name() const override { return "cmd:" + (pc_.model.empty() ? pc_.cmd : pc_.model); }
    std::string model_sha256() const override { return "unknown"; }
    ModelReply complete(const ModelRequest& req) override {
        exec::SessionOptions so;
        so.timeout_s = pc_.timeout_s;
        so.input = req.system + "\n\n" + req.user + "\n";
#ifdef _WIN32
        std::vector<std::string> argv{"cmd", "/c", pc_.cmd};
#else
        std::vector<std::string> argv{"/bin/sh", "-c", pc_.cmd};
#endif
        auto r = exec::run_session(argv, so);
        if (r.failed) return {"", "PRISM_AI_CMD could not start"};
        if (r.timed_out) return {"", "timeout"};
        if (r.rc != 0) return {"", "PRISM_AI_CMD exit " + std::to_string(r.rc) + ": " + r.err.substr(0, 300)};
        return {r.out, ""};
    }

private:
    ProviderConfig pc_;
};

}  // namespace

std::optional<ProviderConfig> provider_from_env(std::string* why) {
    ProviderConfig pc;
    std::string k = env("PRISM_AI");
    for (auto& c : k) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (k.empty()) k = "forge";
    std::string def_url;
    if (k == "forge") {
        pc.kind = "forge";
    } else if (k == "openai") {
        pc.kind = "openai";
        def_url = "https://api.openai.com/v1";
        pc.key_env = "OPENAI_API_KEY";
    } else if (k == "ollama" || k == "vllm" || k == "llamacpp" || k == "llama.cpp" || k == "lmstudio") {
        pc.kind = "openai";
        def_url = k == "ollama" ? "http://localhost:11434/v1"
                  : k == "lmstudio" ? "http://localhost:1234/v1"
                  : k == "vllm" ? "http://localhost:8000/v1"
                                : "http://localhost:8080/v1";
    } else if (k == "anthropic" || k == "claude") {
        pc.kind = "anthropic";
        def_url = "https://api.anthropic.com";
        pc.key_env = "ANTHROPIC_API_KEY";
        pc.model = "claude-opus-5-5";
    } else if (k == "cmd" || k == "command") {
        pc.kind = "cmd";
        pc.cmd = env("PRISM_AI_CMD");
    } else {
        if (why) *why = "PRISM_AI=" + k + " is not a provider (forge, openai, ollama, vllm, llamacpp, lmstudio, anthropic, cmd)";
        return std::nullopt;
    }
    pc.url = env("PRISM_AI_URL").empty() ? def_url : env("PRISM_AI_URL");
    if (!env("PRISM_AI_MODEL").empty()) pc.model = env("PRISM_AI_MODEL");
    if (const char* ke = std::getenv("PRISM_AI_KEY_ENV")) pc.key_env = ke;
    pc.send_grammar = env("PRISM_AI_GRAMMAR") == "1";
    return pc;
}

std::shared_ptr<ModelBackend> provider_backend(const ProviderConfig& pc, std::string* why) {
    auto say = [&](std::string m) {
        if (why) *why = std::move(m);
        return std::shared_ptr<ModelBackend>{};
    };
    if (pc.kind == "forge") return say("FORGE is the provider (PRISM_AI=forge)");
    if (pc.kind == "cmd") {
        if (pc.cmd.empty()) return say("PRISM_AI=cmd needs PRISM_AI_CMD (a command reading the prompt on stdin)");
        return std::make_shared<CmdBackend>(pc);
    }
    if (pc.url.empty()) return say("PRISM_AI=" + pc.kind + " needs PRISM_AI_URL");
    if (pc.model.empty()) return say("PRISM_AI=" + pc.kind + " needs PRISM_AI_MODEL");
    if (!pc.key_env.empty() && env(pc.key_env.c_str()).empty() && pc.kind == "anthropic")
        return say("PRISM_AI=anthropic: the key variable " + pc.key_env + " is not set (PRISM_AI_KEY_ENV names it)");
    return std::make_shared<HttpBackend>(pc);
}

std::string provider_request_json(const ProviderConfig& pc, const ModelRequest& req) {
    json j;
    j["model"] = pc.model;
    j["max_tokens"] = pc.max_tokens;
    if (pc.kind == "anthropic") {
        // API version 2026-10-21: no sampling fields
        if (!req.system.empty()) j["system"] = req.system;
        j["messages"] = json::array({{{"role", "user"}, {"content", req.user}}});
    } else {
        json msgs = json::array();
        if (!req.system.empty()) msgs.push_back({{"role", "system"}, {"content", req.system}});
        msgs.push_back({{"role", "user"}, {"content", req.user}});
        j["messages"] = msgs;
        j["temperature"] = 0.2;
        if (pc.send_grammar && !req.grammar_text.empty()) j["grammar"] = req.grammar_text;  // llama.cpp server
    }
    return j.dump();
}

std::string provider_reply_text(const ProviderConfig& pc, const std::string& body, std::string* err) {
    auto fail = [&](std::string e) {
        if (err) *err = std::move(e);
        return std::string();
    };
    json j = json::parse(body, nullptr, false);
    if (j.is_discarded() || !j.is_object()) return fail("provider answer is not JSON: " + body.substr(0, 200));
    if (j.contains("error")) {
        const auto& e = j["error"];
        return fail("provider error: " + (e.is_object() && e.contains("message") ? e["message"].get<std::string>() : e.dump()));
    }
    if (pc.kind == "anthropic") {
        if (j.value("stop_reason", "") == "refusal") return fail("provider declined the request (refusal)");
        std::string text;
        for (auto& b : j.value("content", json::array()))
            if (b.value("type", "") == "text") text += b.value("text", "");
        if (text.empty()) return fail("provider answer has no text");
        return text;
    }
    try {
        const auto& m = j.at("choices").at(0).at("message");
        if (m.contains("content") && m["content"].is_string()) return m["content"].get<std::string>();
    } catch (const std::exception&) {
    }
    return fail("provider answer has no choices[0].message.content");
}

}  // namespace prism::ai
