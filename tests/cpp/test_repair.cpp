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
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

#ifdef _WIN32
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  ifdef ERROR
#    undef ERROR
#  endif
#  pragma comment(lib, "ws2_32.lib")
#else
#  include <arpa/inet.h>
#  include <netinet/in.h>
#  include <poll.h>
#  include <sys/socket.h>
#  include <unistd.h>
#endif

namespace {

// One loopback socket API for both platforms (Winsock on WIN32, BSD sockets
// elsewhere), so the fake llama-server below is the same code on each.
#ifdef _WIN32
using sock_t = SOCKET;
const sock_t kBadSock = INVALID_SOCKET;
using sock_len_t = int;

struct WinsockInit {
    WinsockInit() {
        WSADATA d;
        ok = ::WSAStartup(MAKEWORD(2, 2), &d) == 0;
    }
    ~WinsockInit() {
        if (ok) ::WSACleanup();
    }
    bool ok = false;
};

void sock_close(sock_t s) { ::closesocket(s); }
bool sock_readable(sock_t s, int ms) {
    WSAPOLLFD p{};
    p.fd = s;
    p.events = POLLRDNORM;
    return ::WSAPoll(&p, 1, ms) > 0;
}
#else
using sock_t = int;
const sock_t kBadSock = -1;
using sock_len_t = socklen_t;

void sock_close(sock_t s) { ::close(s); }
bool sock_readable(sock_t s, int ms) {
    pollfd p{s, POLLIN, 0};
    return ::poll(&p, 1, ms) > 0;
}
#endif

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// A llama-server stand-in on 127.0.0.1: any request answers 200 (so /health
// is up), and POST /v1/chat/completions answers with `patch` as the model
// reply. The whole request (headers and Content-Length body) is read before
// answering: closing a Winsock socket with unread data resets the connection
// and the client would lose the response.
struct FakeLlm {
#ifdef _WIN32
    WinsockInit wsa;
#endif
    sock_t lfd = kBadSock;
    int port = 0;
    std::atomic<bool> stop{false};
    std::thread th;
    std::atomic<int> chats{0};
    std::string patch =
        "```c\nint div_param(int a, int b) {\n    if (b == 0 || (a == -2147483647 - 1 && b == -1)) return 0;\n    "
        "return a / b;\n}\nint main(void) { return div_param(4, 2) - 2; }\n```";

    FakeLlm() {
#ifdef _WIN32
        REQUIRE(wsa.ok);
#endif
        lfd = ::socket(AF_INET, SOCK_STREAM, 0);
        REQUIRE(lfd != kBadSock);
#ifndef _WIN32
        // Not on Winsock: SO_REUSEADDR there lets another process share the port.
        int one = 1;
        ::setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
#endif
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        REQUIRE(::bind(lfd, reinterpret_cast<sockaddr*>(&a), sizeof a) == 0);
        sock_len_t len = sizeof a;
        ::getsockname(lfd, reinterpret_cast<sockaddr*>(&a), &len);
        port = ntohs(a.sin_port);
        REQUIRE(::listen(lfd, 16) == 0);
        th = std::thread([this] { loop(); });
    }
    ~FakeLlm() {
        stop = true;
        th.join();
        sock_close(lfd);
    }
    FakeLlm(const FakeLlm&) = delete;
    FakeLlm& operator=(const FakeLlm&) = delete;

    std::string url() const { return "http://127.0.0.1:" + std::to_string(port); }
    void loop() {
        while (!stop) {
            if (!sock_readable(lfd, 50)) continue;
            sock_t c = ::accept(lfd, nullptr, nullptr);
            if (c == kBadSock) continue;
            serve(c);
            sock_close(c);
        }
    }
    void serve(sock_t c) {
        std::string raw;
        char buf[4096];
        std::size_t need = std::string::npos;
        for (;;) {
            auto he = raw.find("\r\n\r\n");
            if (he != std::string::npos && need == std::string::npos) {
                need = he + 4;
                auto head = lower(raw.substr(0, he));
                auto cl = head.find("content-length:");
                if (cl != std::string::npos) {
                    try {
                        need += std::stoul(head.substr(cl + 15));
                    } catch (...) {
                    }
                }
            }
            if (need != std::string::npos && raw.size() >= need) break;
            if (!sock_readable(c, 2000)) return;
            auto n = ::recv(c, buf, static_cast<int>(sizeof buf), 0);
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
        ::send(c, resp.data(), static_cast<int>(resp.size()), 0);
    }
};

// The engine's own lookup (gcc, clang, cl); rlef_repair gives up without one.
// The candidate is built with gcc/clang-style flags (-std=c11 ... -o), so on
// WIN32 a bare cl is not enough: that is NOTRUN, not a pass.
bool have_cc() {
#ifdef _WIN32
    return prism::Config{}.which({"gcc", "clang"}).has_value();
#else
    return prism::Config{}.which({"gcc", "clang", "cc"}).has_value();
#endif
}

}  // namespace

TEST_CASE("repair: verified fix is HYPOTHESIS with patch_verdict, audit clean") {
    if (!have_cc()) {
        MESSAGE("NOTRUN: no gcc/clang on PATH for the LLM candidate");
        return;
    }
    FakeLlm srv;
    auto td = std::filesystem::temp_directory_path() / "prism_repair_test";
    std::error_code ec;
    std::filesystem::remove_all(td, ec);
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
    cfg.allow_exec = true;  // Law 9: the LLM-written candidate runs only when asked
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
                std::filesystem::remove_all(td, ec);
                return;
            }
    }
    REQUIRE(fix != nullptr);
    CHECK(srv.chats >= 1);
    CHECK(fix->status == std::string(prism::laws::HYPOTHESIS));
    CHECK(fix->strength == std::string(prism::laws::STRENGTH_READS));
    CHECK(prism::laws::is_proof(fix->extra.at("patch_verdict")));
    CHECK(fix->extra.at("fix_label") == "verified fix");
    CHECK(fix->message.find("not the scanned code") != std::string::npos);
    for (auto& f : rows) CHECK_FALSE(prism::laws::is_proof(f.status));
    std::filesystem::remove_all(td, ec);
}

TEST_CASE("repair: without --allow-exec the candidate never runs (NOTRUN, Law 9)") {
    FakeLlm srv;
    auto td = std::filesystem::temp_directory_path() / "prism_repair_noexec";
    std::error_code ec;
    std::filesystem::remove_all(td, ec);
    auto src = td / "src";
    std::filesystem::create_directories(src);
    std::ofstream(src / "div_param.c") << "int div_param(int a, int b) {\n    return a / b;\n}\n";
    prism::Finding fail;
    fail.stage = "bmc";
    fail.status = prism::laws::FAILED;
    fail.file = (src / "div_param.c").string();
    fail.function = "div_param";
    fail.cls = "INT-DIV-ZERO";
    prism::Config cfg = prism::default_config();
    cfg.root = src;
    cfg.out = td / "out";
    cfg.allow_exec = false;
    cfg.gguf = "/nonexistent/prism-test.gguf";
    cfg.ollama_host.clear();
    cfg.llama_server = srv.url();
    auto rows = prism::rlef_repair(fail, cfg);
    REQUIRE(rows.size() == 1);
    // Either the compiler is missing (NOTRUN, Law 1) or exec is off (NOTRUN, Law 9):
    // never a verdict, never a patch, and the model is not asked to run anything.
    CHECK(rows[0].status == std::string(prism::laws::NOTRUN));
    CHECK_FALSE(rows[0].extra.contains("patch_verdict"));
    CHECK_FALSE(prism::laws::is_proof(rows[0].status));
    CHECK(srv.chats == 0);
    std::filesystem::remove_all(td, ec);
}
