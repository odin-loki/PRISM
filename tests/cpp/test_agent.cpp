// The model-driven stage pieces (src/prism/stages/: llm, hypothesize,
// execute_cex, rlef, fuse): the chat engine and its test seam, reply parsing,
// the sandbox verdict and RLEF reward, the interpreter and repair loops with a
// replaceable model and BMC oracle, counterexample replay parsing, and the
// Dafny-style contract hypotheses. Model output never proves anything about
// the scanned code (Law 4); a missing model or compiler is NOTRUN (Law 1).

#include <doctest/doctest.h>
#ifdef ERROR
#  undef ERROR
#endif

#include "../../src/prism/stages/agent.hpp"

#include "prism/config.hpp"
#include "prism/cparse.hpp"
#include "prism/laws.hpp"
#include "prism/pipeline.hpp"
#include "prism/stages.hpp"

#include <nlohmann/json.hpp>

#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifndef _WIN32
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;
namespace sd = prism::stages_detail;
using json = nlohmann::json;

namespace {

using Messages = std::vector<std::pair<std::string, std::string>>;

// Installs a chat function for one scope.
struct ChatDouble {
    std::vector<Messages> seen;
    std::vector<sd::ChatResult> replies;
    std::size_t next = 0;
    std::mutex mu;
    ChatDouble() {
        sd::set_chat_backend_for_testing([this](const Messages& m, double) {
            std::lock_guard<std::mutex> g(mu);
            seen.push_back(m);
            if (replies.empty()) return sd::ChatResult{"", "test-double", "no reply scripted"};
            auto r = replies[std::min(next, replies.size() - 1)];
            ++next;
            if (r.backend.empty()) r.backend = "test-double";
            return r;
        });
    }
    ~ChatDouble() { sd::set_chat_backend_for_testing(nullptr); }
};

fs::path agent_tmp(const std::string& tag) {
    auto p = fs::temp_directory_path() / ("prism-agent-" + tag);
    std::error_code ec;
    fs::remove_all(p, ec);
    fs::create_directories(p);
    return p;
}

prism::Config offline_cfg(const fs::path& out) {
    prism::Config cfg = prism::default_config();
    cfg.out = out;
    cfg.llama_server = "http://127.0.0.1:1";
    cfg.ollama_host = "http://127.0.0.1:1";
    cfg.gguf.clear();
    return cfg;
}

bool have_cc() { return sd::which_cc().has_value(); }

#ifndef _WIN32
// A one-route HTTP server: every request gets the answer of `handler`
// (status, body) for its (method, path).
class TinyHttp {
public:
    using Handler = std::function<std::pair<int, std::string>(const std::string& method, const std::string& path)>;
    explicit TinyHttp(Handler h) : handler_(std::move(h)) {
        fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        int one = 1;
        ::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        ::bind(fd_, reinterpret_cast<sockaddr*>(&a), sizeof a);
        ::listen(fd_, 16);
        socklen_t len = sizeof a;
        ::getsockname(fd_, reinterpret_cast<sockaddr*>(&a), &len);
        port_ = ntohs(a.sin_port);
        th_ = std::thread([this] {
            while (!stop_) {
                pollfd p{fd_, POLLIN, 0};
                if (::poll(&p, 1, 100) <= 0) continue;
                int c = ::accept(fd_, nullptr, nullptr);
                if (c < 0) continue;
                serve(c);
                ::close(c);
            }
        });
    }
    ~TinyHttp() {
        stop_ = true;
        th_.join();
        ::close(fd_);
    }
    std::string url() const { return "http://127.0.0.1:" + std::to_string(port_); }

private:
    Handler handler_;
    int fd_ = -1, port_ = 0;
    std::atomic<bool> stop_{false};
    std::thread th_;
    void serve(int c) {
        std::string req;
        char buf[8192];
        std::size_t need = 0, head = std::string::npos;
        while (true) {
            pollfd p{c, POLLIN, 0};
            if (::poll(&p, 1, 3000) <= 0) break;
            auto n = ::recv(c, buf, sizeof buf, 0);
            if (n <= 0) break;
            req.append(buf, static_cast<std::size_t>(n));
            if (head == std::string::npos && (head = req.find("\r\n\r\n")) != std::string::npos) {
                std::string low;
                for (char ch : req.substr(0, head)) low += static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                auto cl = low.find("content-length:");
                need = cl == std::string::npos ? 0 : std::stoul(low.substr(cl + 15));
            }
            if (head != std::string::npos && req.size() >= head + 4 + need) break;
        }
        const auto sp1 = req.find(' '), sp2 = req.find(' ', sp1 + 1);
        if (sp1 == std::string::npos || sp2 == std::string::npos) return;
        auto [code, body] = handler_(req.substr(0, sp1), req.substr(sp1 + 1, sp2 - sp1 - 1));
        std::string resp = "HTTP/1.1 " + std::to_string(code) + " X\r\nContent-Type: application/json\r\nContent-Length: " +
                           std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
        ::send(c, resp.data(), resp.size(), MSG_NOSIGNAL);
    }
};
#endif

}  // namespace

// ------------------------------------------------------------------ reply parsing
TEST_CASE("agent: the C file of a model reply") {
    CHECK(sd::c_from_llm("```int main(){}```").empty());  // a fence line with no newline holds no file
    CHECK(sd::c_from_llm("```c\nint x;\n```") == "int x;");
    CHECK(sd::c_from_llm("Here it is:\n```c\nint y;\n```\nDone.") == "int y;");
    CHECK(sd::c_from_llm("int z;") == "int z;");
    CHECK(sd::c_from_llm("  \n ").empty());
    CHECK(sd::c_from_llm("see ```inline``` here") == "inline");
}

TEST_CASE("agent: hex seeds are read like bytes.fromhex after dropping 0x") {
    using B = std::vector<uint8_t>;
    CHECK(sd::parse_hex_bytes("0x01 02") == B{1, 2});
    CHECK(sd::parse_hex_bytes("0X0a0B") == B{10, 11});
    CHECK(sd::parse_hex_bytes("de ad\tbe\nef") == B{0xde, 0xad, 0xbe, 0xef});
    CHECK(sd::parse_hex_bytes("0x10 0x20") == B{0x10, 0x20});
    CHECK_FALSE(sd::parse_hex_bytes("0 1"));  // a byte is never split
    CHECK_FALSE(sd::parse_hex_bytes("abc"));
    CHECK_FALSE(sd::parse_hex_bytes("zz"));
    CHECK(sd::parse_hex_bytes("") == B{});
    const auto data = json::parse(R"({"seeds": ["0102", 1234, "", "zz", "0x ff", null]})");
    CHECK(sd::json_hex_list(data, "seeds") == std::vector<B>{{1, 2}, {0x12, 0x34}, {0xff}});
    CHECK(sd::json_hex_list(json::parse(R"({"seeds": "0102"})"), "seeds").empty());
    CHECK(sd::score_prompt_seeds({{1, 2, 3}, {1, 2}, {1, 2, 9}, {}}, 2) == 1);
}

TEST_CASE("agent: documentation comments leave out contracts and ACSL") {
    const std::string src =
        "/* requires : x > 0 */\n"
        "// diff: old behaviour\n"
        "/** Adds\n *  two   numbers\n **/\n"
        "// Ensures: nothing\n"
        "// plain note\n"
        "/*@ ensures \\result > 0; */\n"
        "int add(int a, int b) { return a + b; }\n";
    CHECK(sd::documentation_from_comments(src) == "Adds two numbers\nplain note");
    std::string many;
    for (int i = 0; i < 12; ++i) many += "// note " + std::to_string(i) + "\n";
    const auto d = sd::documentation_from_comments(many);
    CHECK(d.find("note 7") != std::string::npos);
    CHECK(d.find("note 8") == std::string::npos);  // at most 8 comments
}

// ------------------------------------------------------------------ verdicts and rewards
TEST_CASE("agent: the sandbox verdict never proves; a missing compiler is NOTRUN") {
    CHECK(sd::sandbox_verdict({{"ok", false}, {"error", "no compiler"}}) == prism::laws::NOTRUN);
    CHECK(sd::sandbox_verdict({{"ok", false}, {"error", "exec-disabled"}}) == prism::laws::NOTRUN);
    CHECK(sd::sandbox_verdict({{"ok", true}, {"error", nullptr}}) == prism::laws::CLEAN);
    CHECK(sd::sandbox_verdict({{"ok", false}, {"error", "crash"}}) == prism::laws::CRASH);
    CHECK(sd::sandbox_verdict({{"ok", false}, {"error", "exit"}}) == prism::laws::FAILED);
    CHECK(sd::sandbox_verdict({{"ok", false}, {"error", "compile"}}) == prism::laws::FAILED);
    const auto off = sd::sandbox_run("int main(void){return 0;}", 2.0, /*allow_exec=*/false);
    CHECK(off["error"] == "exec-disabled");  // Law 9
}

TEST_CASE("agent: the RLEF reward; only a BMC proof of the patch is terminal, BOUNDED is not") {
    auto r = sd::rlef_reward({{"ok", false}, {"error", "compile"}}, "");
    CHECK(r.score == 0);
    CHECK(r.proved.empty());
    CHECK(sd::rlef_reward({{"ok", true}, {"error", nullptr}}, "").score == 3);
    CHECK(sd::rlef_reward({{"ok", false}, {"error", "crash"}}, "").score == 0);
    CHECK(sd::rlef_reward({{"ok", false}, {"error", "exit"}}, prism::laws::FAILED).score == -1);
    CHECK(sd::rlef_reward({{"ok", true}, {"error", nullptr}}, prism::laws::PROVED).proved == prism::laws::PROVED);
    auto b = sd::rlef_reward({{"ok", true}, {"error", nullptr}}, prism::laws::BOUNDED);
    CHECK(b.proved.empty());
    CHECK(b.score == 3);
    CHECK(sd::rlef_reward({{"ok", true}, {"error", nullptr}}, prism::laws::CLEAN).proved.empty());
}

// ------------------------------------------------------------------ the chat engine
TEST_CASE("agent: the chat seam binds every engine; unreachable servers are NOTRUN, not ERROR") {
    auto out = agent_tmp("engine");
    {
        sd::LlamaEngine none(offline_cfg(out));
        CHECK_FALSE(none.available());
        auto r = none.complete({{"user", "hi"}});
        CHECK_FALSE(r.error.empty());
        CHECK(sd::llm_httpish(r.error));
    }
    ChatDouble chat;
    chat.replies.push_back({"hello", "", "", json{{"id", 1}}});
    sd::LlamaEngine e(offline_cfg(out));
    REQUIRE(e.available());
    CHECK(e.backend == "test-double");
    auto r = e.complete({{"system", "s"}, {"user", "u"}});
    CHECK(r.text == "hello");
    CHECK(r.raw["id"] == 1);
    REQUIRE(chat.seen.size() == 1);
    CHECK(chat.seen[0][1].second == "u");
}

#ifndef _WIN32
TEST_CASE("agent: an HTTP error keeps its status and body; a good answer keeps its JSON") {
    auto out = agent_tmp("http");
    TinyHttp srv([](const std::string& method, const std::string& path) -> std::pair<int, std::string> {
        if (method == "GET") return {200, "{}"};
        if (path == "/v1/chat/completions") return {503, R"({"error":"model is loading"})"};
        return {404, "no"};
    });
    auto cfg = offline_cfg(out);
    cfg.llama_server = srv.url();
    sd::LlamaEngine e(cfg);
    REQUIRE(e.backend == "llama-server");
    auto r = e.complete({{"user", "hi"}}, 5.0);
    CHECK(r.error == R"(HTTP 503: {"error":"model is loading"})");
    CHECK(sd::llm_httpish(r.error));  // a server that cannot answer is a missing backend (NOTRUN)
    TinyHttp ok([](const std::string& method, const std::string&) -> std::pair<int, std::string> {
        if (method == "GET") return {200, "{}"};
        return {200, R"({"choices":[{"message":{"content":"fine"}}],"usage":{"total_tokens":3}})"};
    });
    cfg.llama_server = ok.url();
    sd::LlamaEngine e2(cfg);
    auto r2 = e2.complete({{"user", "hi"}}, 5.0);
    CHECK(r2.error.empty());
    CHECK(r2.text == "fine");
    CHECK(r2.raw["usage"]["total_tokens"] == 3);
}

TEST_CASE("agent: Fuzz4All uses the run's model settings, not built-in defaults") {
    auto td = agent_tmp("fuse-cfg");
    std::ofstream(td / "f.c") << "int f(int x) {\n    return x + 1;\n}\n";
    auto fns = prism::extract_functions(td / "f.c", "f.c");
    REQUIRE(!fns.empty());
    TinyHttp srv([](const std::string& method, const std::string&) -> std::pair<int, std::string> {
        if (method == "GET") return {200, "{}"};
        return {200, R"({"choices":[{"message":{"content":"{\"seeds\":[\"01000000\"]}"}}]})"};
    });
    auto cfg = offline_cfg(td / "out");
    cfg.llama_server = srv.url();
    auto has_skip = [](const std::vector<prism::Finding>& fs) {
        for (const auto& f : fs)
            if (f.message.find("stall mutants / autoprompt skipped") != std::string::npos) return true;
        return false;
    };
    // The run's configuration reaches the model; without it the defaults
    // (here: nothing listening) say NOTRUN.
    ::setenv("PRISM_LLAMA_SERVER", "http://127.0.0.1:1", 1);
    ::setenv("OLLAMA_HOST", "http://127.0.0.1:1", 1);
    CHECK_FALSE(has_skip(prism::run_fuse(fns, {}, td, 0.05, 2, true, &cfg)));
    CHECK(has_skip(prism::run_fuse(fns, {}, td, 0.05, 2, true)));
    ::unsetenv("PRISM_LLAMA_SERVER");
    ::unsetenv("OLLAMA_HOST");
}
#endif

// ------------------------------------------------------------------ interpreter and repair loops
TEST_CASE("agent: the interpreter loop runs model C only with --allow-exec; CLEAN and CRASH carry the sandbox") {
    auto out = agent_tmp("interp");
    ChatDouble chat;
    chat.replies.push_back({"```c\n#include <stdio.h>\nint main(void) { puts(\"hi\"); return 0; }\n```", "", ""});
    sd::LlamaEngine e(offline_cfg(out));
    auto off = sd::interpreter_loop(e, "prove it", 2, /*allow_exec=*/false);
    REQUIRE(off.size() == 1);
    CHECK(off[0].status == prism::laws::NOTRUN);
    CHECK(chat.seen.empty());  // Law 9: not even asked
    if (!have_cc()) {
        auto nc = sd::interpreter_loop(e, "prove it", 2, true);
        CHECK(nc[0].status == prism::laws::NOTRUN);
        return;
    }
    auto clean = sd::interpreter_loop(e, "prove it", 2, true);
    REQUIRE(clean.size() == 1);
    CAPTURE(clean[0].message);
    CHECK(clean[0].status == prism::laws::CLEAN);
    CHECK(clean[0].message.find("not a proof") != std::string::npos);
    CHECK(clean[0].extra.at("stdout") == "hi\n");
    CHECK(clean[0].extra.count("sandbox") == 1);
    chat.replies = {{"```c\n#include <signal.h>\nint main(void) { raise(SIGSEGV); return 0; }\n```", "", ""}};
    chat.next = 0;
    auto crash = sd::interpreter_loop(e, "prove it", 2, true);
    REQUIRE(crash.size() == 1);
    CHECK(crash[0].status == prism::laws::CRASH);
    CHECK(crash[0].extra.count("code") == 1);
    CHECK(crash[0].extra.count("sandbox") == 1);
    // A model that errors: HTTP-ish is NOTRUN, anything else is ERROR.
    chat.replies = {{"", "", "HTTP 503: busy"}};
    chat.next = 0;
    CHECK(sd::interpreter_loop(e, "p", 1, true)[0].status == prism::laws::NOTRUN);
    chat.replies = {{"", "", "timed out"}};
    chat.next = 0;
    CHECK(sd::interpreter_loop(e, "p", 1, true)[0].status == prism::laws::ERROR);
}

TEST_CASE("agent: RLEF repair with a replaceable BMC oracle; a proof is about the patch only") {
    auto td = agent_tmp("rlef");
    std::ofstream(td / "div.c") << "int f(int a, int b) {\n    return a / b;\n}\n";
    prism::Finding fail;
    fail.stage = "bmc";
    fail.status = std::string(prism::laws::FAILED);
    fail.file = (td / "div.c").string();
    fail.function = "f";
    fail.line = 1;
    fail.cls = "INT-DIV-ZERO";
    fail.message = "div0";
    auto cfg = offline_cfg(td / "out");
    cfg.repair_rounds = 1;
    ChatDouble chat;
    chat.replies.push_back(
        {"```c\nint f(int a, int b) {\n    return b == 0 ? 0 : a / b;\n}\nint main(void) { return f(1, 0); }\n```", "",
         ""});
    // Without --allow-exec nothing runs (Law 9).
    auto off = sd::rlef_repair_with(fail, cfg, [](const std::string&) { return std::string("PROVED"); });
    REQUIRE(off.size() == 1);
    CHECK(off[0].status == prism::laws::NOTRUN);
    if (!have_cc()) return;
    cfg.allow_exec = true;
    std::vector<std::string> asked;
    auto proved = sd::rlef_repair_with(fail, cfg, [&](const std::string& src) {
        asked.push_back(src);
        return std::string(prism::laws::PROVED);
    });
    REQUIRE(proved.size() == 1);
    CHECK(asked.size() == 1);
    CHECK(proved[0].status == prism::laws::HYPOTHESIS);  // Law 4: never a verdict on the scanned code
    CHECK(proved[0].strength == prism::laws::STRENGTH_READS);
    CHECK(proved[0].extra.at("patch_verdict") == "PROVED");
    chat.next = 0;
    auto bounded = sd::rlef_repair_with(fail, cfg, [](const std::string&) { return std::string("BOUNDED"); });
    REQUIRE(bounded.size() == 1);
    CHECK(bounded[0].status == prism::laws::CLEAN);  // ran and passed: CLEAN, never PROVED
    CHECK(bounded[0].message.find("not a proof") != std::string::npos);
    chat.next = 0;
    auto thrown = sd::rlef_repair_with(fail, cfg, [](const std::string&) -> std::string { throw std::runtime_error("x"); });
    REQUIRE(thrown.size() == 1);
    CHECK(thrown[0].status != prism::laws::PROVED);
}

// ------------------------------------------------------------------ counterexample replay
TEST_CASE("agent: counterexample values are read like int(v, 0); others are not replayed") {
    auto td = agent_tmp("cex");
    std::ofstream(td / "d.c") << "int f(int a, int b) {\n    return 100 / (b - 16);\n}\n";
    auto fns = prism::extract_functions(td / "d.c", "d.c");
    REQUIRE(fns.size() == 1);
    REQUIRE(fns[0].kind == "SCALAR");
    auto cfg = offline_cfg(td / "out");
    cfg.llm = false;
    auto replay = [&](const std::string& cex) {
        prism::Finding f;
        f.stage = "bmc";
        f.status = std::string(prism::laws::FAILED);
        f.file = "d.c";
        f.function = "f";
        f.line = 1;
        f.cls = "INT-DIV-ZERO";
        f.counterexample = cex;
        return prism::execute_cex({f}, fns, cfg);
    };
    // "010" is refused (not octal 8); b = 0x10 = 16 divides by zero.
    auto a = replay("a=010, b=0x10");
    REQUIRE(a.size() == 1);
    CHECK(a[0].status == prism::laws::CRASH);
    CHECK(replay("int b =\t16")[0].status == prism::laws::CRASH);  // whitespace of any kind
    CHECK(replay("b=0b1_0000")[0].status == prism::laws::CRASH);
    CHECK(replay("b=17")[0].status == prism::laws::CLEAN);
    // An unsigned 32-bit pattern is replayed as its bits; beyond 32 bits nothing is.
    CHECK(replay("b=4294967295")[0].status == prism::laws::CLEAN);
    auto big = replay("b=99999999999");
    REQUIRE(big.size() == 1);
    CHECK(big[0].status == prism::laws::NOTRUN);
    CHECK(big[0].message == "no FAILED/CRASH cex to replay");
    CHECK(replay("b=010")[0].status == prism::laws::NOTRUN);
}

// ------------------------------------------------------------------ Dafny-style contracts
TEST_CASE("agent: Dafny-style specs are hypotheses; no model is NOTRUN") {
    auto td = agent_tmp("dafny");
    std::ofstream(td / "c.c") << "int inc(int x) {\n    return x + 1;\n}\nvoid nop(void) {\n}\n";
    auto fns = prism::extract_functions(td / "c.c", "c.c");
    REQUIRE(fns.size() == 2);
    auto cfg = offline_cfg(td / "out");
    auto none = prism::dafny_specs(fns, 3, cfg);
    REQUIRE(none.size() == 1);
    CHECK(none[0].stage == "contracts");
    CHECK(none[0].status == prism::laws::NOTRUN);
    CHECK(none[0].cls == "FUNC-CONTRACT");
    CHECK(none[0].message == "llama.cpp/Ollama not reachable");
    ChatDouble chat;
    chat.replies = {{R"({"requires":["x < 2147483647"],"ensures":["\\result == x + 1"]})", "", ""},
                    {"", "", "HTTP 500: oops"}};
    auto specs = prism::dafny_specs(fns, 3, cfg);
    REQUIRE(specs.size() == 2);
    CHECK(specs[0].status == prism::laws::HYPOTHESIS);
    CHECK(specs[0].strength == prism::laws::STRENGTH_READS);
    CHECK(specs[0].message == "Dafny-style spec (hypothesis, not proved)");
    CHECK(json::parse(specs[0].extra.at("spec"))["requires"][0] == "x < 2147483647");
    CHECK(specs[0].extra.at("backend") == "test-double");
    CHECK(specs[1].status == prism::laws::NOTRUN);
    CHECK(specs[1].extra.count("install") == 1);
    CHECK(prism::dafny_specs(fns, 0, cfg).empty());
}

TEST_CASE("agent: the contracts stage adds the model's specs beside its proofs, as hypotheses") {
    auto td = agent_tmp("contracts-stage");
    fs::create_directories(td / "src");
    std::ofstream(td / "src" / "c.c") << "int inc(int x) {\n    return x + 1;\n}\n";
    ChatDouble chat;
    chat.replies = {{R"({"requires":["x < 100"]})", "", ""}};
    auto cfg = offline_cfg(td / "out");
    cfg.root = td / "src";
    cfg.llm = true;
    cfg.stages = std::vector<std::string>{"inventory", "classify", "contracts"};
    auto rep = prism::run_pipeline(cfg);
    bool hyp = false;
    for (const auto& s : rep.stages)
        if (s.name == "contracts")
            for (const auto& f : s.findings)
                if (f.message == "Dafny-style spec (hypothesis, not proved)") {
                    hyp = true;
                    CHECK(f.status == prism::laws::HYPOTHESIS);
                }
    CHECK(hyp);
    cfg.llm = false;
    cfg.out = td / "out2";
    auto rep2 = prism::run_pipeline(cfg);
    for (const auto& s : rep2.stages)
        if (s.name == "contracts")
            for (const auto& f : s.findings) CHECK(f.message != "Dafny-style spec (hypothesis, not proved)");
}
