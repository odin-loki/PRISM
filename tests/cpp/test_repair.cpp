// RLEF repair: patch_verdict is HYPOTHESIS / READS about the LLM patch (Law 4).
// Port of tests/test_repair_verdict.py (C++ binary + fake llama-server).

#include <doctest/doctest.h>
#ifdef ERROR
#  undef ERROR
#endif

#include "prism/config.hpp"
#include "prism/laws.hpp"
#include "prism/stages.hpp"

#include <nlohmann/json.hpp>

#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

#ifndef _WIN32
#  include <arpa/inet.h>
#  include <netinet/in.h>
#  include <poll.h>
#  include <sys/socket.h>
#  include <unistd.h>

namespace {

struct FakeLlm {
    int lfd = -1;
    int port = 0;
    std::atomic<bool> stop{false};
    std::thread th;
    std::atomic<int> chats{0};
    std::string patch =
        "```c\nint div_param(int a, int b) {\n    if (b == 0 || (a == -2147483647 - 1 && b == -1)) return 0;\n    "
        "return a / b;\n}\nint main(void) { return div_param(4, 2) - 2; }\n```";

    FakeLlm() {
        lfd = ::socket(AF_INET, SOCK_STREAM, 0);
        int one = 1;
        ::setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        REQUIRE(::bind(lfd, reinterpret_cast<sockaddr*>(&a), sizeof a) == 0);
        socklen_t len = sizeof a;
        ::getsockname(lfd, reinterpret_cast<sockaddr*>(&a), &len);
        port = ntohs(a.sin_port);
        REQUIRE(::listen(lfd, 16) == 0);
        th = std::thread([this] { loop(); });
    }
    ~FakeLlm() {
        stop = true;
        th.join();
        ::close(lfd);
    }
    std::string url() const { return "http://127.0.0.1:" + std::to_string(port); }
    void loop() {
        while (!stop) {
            pollfd p{lfd, POLLIN, 0};
            if (::poll(&p, 1, 50) <= 0) continue;
            int c = ::accept(lfd, nullptr, nullptr);
            if (c < 0) continue;
            serve(c);
            ::close(c);
        }
    }
    void serve(int c) {
        std::string raw;
        char buf[4096];
        while (raw.find("\r\n\r\n") == std::string::npos) {
            auto n = ::recv(c, buf, sizeof buf, 0);
            if (n <= 0) return;
            raw.append(buf, static_cast<std::size_t>(n));
        }
        std::string body = R"({"choices":[{"message":{"content":""}}]})";
        if (raw.find("/v1/chat/completions") != std::string::npos) {
            chats++;
            body = nlohmann::json{{"choices", {{{"message", {{"content", patch}}}}}}}.dump();
        }
        auto resp = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " +
                    std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
        ::send(c, resp.data(), resp.size(), 0);
    }
};

bool have_cc() {
    return std::system("cc -v >/dev/null 2>&1") == 0 || std::system("gcc -v >/dev/null 2>&1") == 0 ||
           std::system("clang -v >/dev/null 2>&1") == 0;
}

}  // namespace
#endif

TEST_CASE("repair: verified fix is HYPOTHESIS with patch_verdict, audit clean") {
#ifndef _WIN32
    if (!have_cc()) {
        MESSAGE("NOTRUN: no C compiler");
        return;
    }
    FakeLlm srv;
    auto td = std::filesystem::temp_directory_path() / "prism_repair_test";
    std::filesystem::remove_all(td);
    auto src = td / "src";
    std::filesystem::create_directories(src);
    std::ofstream(src / "div_param.c") << "int div_param(int a, int b) {\n    return a / b;\n}\n";
    prism::Finding fail;
    fail.stage = "bmc";
    fail.status = prism::laws::FAILED;
    fail.file = (src / "div_param.c").string();
    fail.function = "div_param";
    fail.cls = "INT-DIV-ZERO";
    fail.message = "div by zero";
    prism::Config cfg = prism::default_config();
    cfg.root = src;
    cfg.out = td / "out";
    cfg.allow_exec = true;
    cfg.gguf = "/nonexistent/prism-test.gguf";
    cfg.ollama_host.clear();
    cfg.llama_server = srv.url();
    auto rows = prism::rlef_repair(fail, cfg);
    REQUIRE_FALSE(rows.empty());
    const prism::Finding* fix = nullptr;
    for (auto& f : rows)
        if (f.extra.contains("patch_verdict")) fix = &f;
    if (!fix) {
        for (auto& f : rows)
            if (f.message.find("sandbox") != std::string::npos || f.message.find("bwrap") != std::string::npos) {
                MESSAGE("NOTRUN: sandbox could not run candidate");
                return;
            }
    }
    REQUIRE(fix != nullptr);
    CHECK(fix->status == std::string(prism::laws::HYPOTHESIS));
    CHECK(fix->strength == std::string(prism::laws::STRENGTH_READS));
    CHECK(prism::laws::is_proof(fix->extra["patch_verdict"]));
    CHECK(fix->extra["fix_label"] == "verified fix");
    CHECK(fix->message.find("not the scanned code") != std::string::npos);
    for (auto& f : rows) CHECK_FALSE(prism::laws::is_proof(f.status));
    std::filesystem::remove_all(td);
#else
    MESSAGE("NOTRUN: fake HTTP repair test is Unix-only");
#endif
}
