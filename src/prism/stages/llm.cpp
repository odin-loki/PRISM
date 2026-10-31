// The LLM chat engine (FORGE mind, in process, on a .fpk model) and the
// sandboxed compile+run of LLM-written C (declared in llm.hpp).
#include "llm.hpp"

#include "prism/ai.hpp"

#include "prism/forge_llm.hpp"

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
        while (!text.empty() && text.front() == '`') text.erase(0, 1);
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
                return nullptr;
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

namespace stages_detail {
LlmEngine::LlmEngine(Config c) : cfg(std::move(c)) { bind(); }
void LlmEngine::bind() {
    if (chat_backend()) {
        backend = "test-double";
        return;
    }
    if (forge_llm::is_fpk(cfg.fpk)) {
        backend = "forge";
        return;
    }
    backend = "none";
}
bool LlmEngine::available() const {
    if (backend != "none") return true;
    if (ai::session_config()) {
        std::string why;
        return ai::session_backend(&why) != nullptr;
    }
    return false;
}
// Roadmap 9.6: every model interaction goes to <out>/ai_audit.jsonl.
// These chats never set a verdict by themselves (checker "none"); a caller
// that checks the output (RLEF BMC) logs its own checked record.
ChatResult LlmEngine::complete(const std::vector<std::pair<std::string, std::string>>& messages, double timeout) {
    auto r = complete_raw(messages, timeout);
    std::string prompt;
    for (auto& [role, content] : messages) prompt += role + ":\n" + content + "\n";
    ai::AuditRecord rec;
    rec.feature = "chat";
    rec.model = r.backend + ":" + cfg.model;
    rec.model_sha256 = "unknown";
    if (r.backend == "forge") {
        auto d = forge_llm::model_digest(cfg.fpk);
        if (!d.empty()) rec.model_sha256 = d;
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
ChatResult LlmEngine::complete_raw(const std::vector<std::pair<std::string, std::string>>& messages, double timeout) {
    if (ai::session_config()) {
        std::string why;
        if (auto injected = ai::session_backend(&why)) {
            std::string system;
            std::string user;
            for (auto& [role, content] : messages) {
                if (role == "system")
                    system = content;
                else if (user.empty())
                    user = content;
                else
                    user += "\n\n" + role + ":\n" + content;
            }
            ai::ModelRequest req;
            req.feature = "chat";
            req.system = system;
            req.user = user;
            auto reply = injected->complete(req);
            return {reply.text, injected->name(), reply.error};
        }
    }
    if (backend == "none")
        return {"", "none", "no FORGE model (set PRISM_FPK to a .fpk)"};
    if (backend == "test-double") {
        if (auto fn = chat_backend()) return fn(messages, timeout);
        return {"", "test-double", "test chat backend cleared"};
    }
    forge_llm::Request req;
    req.messages = messages;
    req.timeout_s = timeout;
    auto rep = forge_llm::complete(cfg.fpk, req);
    nlohmann::json raw{{"prompt_tokens", rep.prompt_tokens}, {"new_tokens", rep.new_tokens}};
    return {rep.text, "forge", rep.error, raw};
}

// Law 9: the program is LLM-written; it runs only with --allow-exec and then
// inside the sandbox (bubblewrap when available, rlimits always).
std::string tail(const std::string& s, std::size_t n) {
    if (s.size() <= n) return s;
    std::size_t b = s.size() - n;
    while (b < s.size()) {
        const unsigned char bc = static_cast<unsigned char>(s.at(b));
        if ((bc & 0xC0u) != 0x80u) break;
        ++b;
    }
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
        if (!out.is_open())
            return {{"ok", false}, {"error", "cannot write source"}, {"stdout", ""}, {"stderr", ""}, {"code", nullptr}};
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
    suppress_crash_dialogs();  // a crashing child must not hang on a Windows error dialog
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
        while (a < b && src.at(a) == '`') ++a;
        while (b > a && src.at(b - 1) == '`') --b;
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
