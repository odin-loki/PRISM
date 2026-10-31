// RLEF repair: patch_verdict is HYPOTHESIS / READS about the LLM patch (Law 4).
// Port of tests/test_repair_verdict.py (C++ binary + fake llama-server).

#include <doctest/doctest.h>
#ifdef ERROR
#  undef ERROR
#endif

#include "forge_test_model.hpp"
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


std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}


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



// ---- FORGE-era replacements for the llama-server fake (ScriptedChat seam) ----
namespace {
const char* kDivFix = "```c\nint div_param(int a, int b) {\n    if (b == 0) return 0;\n    return a / b;\n}\n```\n";
struct RepairCase {
    std::filesystem::path td;
    prism::Finding fail;
    prism::Config cfg = prism::default_config();
    explicit RepairCase(const char* name, bool allow_exec) {
        td = std::filesystem::temp_directory_path() / name;
        std::error_code ec;
        std::filesystem::remove_all(td, ec);
        auto src = td / "src";
        std::filesystem::create_directories(src);
        std::ofstream(src / "div_param.c") << "int div_param(int a, int b) {\n    return a / b;\n}\n";
        fail.stage = "pir";
        fail.status = prism::laws::FAILED;
        fail.file = (src / "div_param.c").string();
        fail.function = "div_param";
        fail.cls = "INT-DIV-ZERO";
        fail.message = "div by zero";
        cfg.root = src;
        cfg.out = td / "out";
        cfg.allow_exec = allow_exec;  // Law 9
        cfg.fpk.clear();
    }
    ~RepairCase() {
        std::error_code ec;
        std::filesystem::remove_all(td, ec);
    }
};
}  // namespace

TEST_CASE("repair: verified fix is HYPOTHESIS with patch_verdict, audit clean") {
#ifndef PRISM_HAS_Z3
    MESSAGE("NOTRUN: PRISM_HAS_Z3 off, no verdict on the candidate");
    return;
#endif
    if (!have_cc()) {
        MESSAGE("NOTRUN: no gcc/clang on PATH for the LLM candidate");
        return;
    }
    prism_test::ScriptedChat llm([](const std::string&) { return std::string(kDivFix); });
    RepairCase rc("prism_repair_test", true);
    auto rows = prism::rlef_repair(rc.fail, rc.cfg);
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
    CHECK(llm.chats() >= 1);
    CHECK(fix->status == std::string(prism::laws::HYPOTHESIS));
    CHECK(fix->strength == std::string(prism::laws::STRENGTH_READS));
    CHECK(prism::laws::is_proof(fix->extra.at("patch_verdict")));
    CHECK(fix->extra.at("fix_label") == "verified fix");
    CHECK(fix->message.find("not the scanned code") != std::string::npos);
    for (auto& f : rows) CHECK_FALSE(prism::laws::is_proof(f.status));
}

TEST_CASE("repair: without --allow-exec the candidate never runs (NOTRUN, Law 9)") {
    prism_test::ScriptedChat llm([](const std::string&) { return std::string(kDivFix); });
    RepairCase rc("prism_repair_noexec", false);
    auto rows = prism::rlef_repair(rc.fail, rc.cfg);
    REQUIRE(rows.size() == 1);
    CHECK(rows[0].status == std::string(prism::laws::NOTRUN));
    CHECK_FALSE(rows[0].extra.contains("patch_verdict"));
    CHECK_FALSE(prism::laws::is_proof(rows[0].status));
    CHECK(llm.chats() == 0);
}

TEST_CASE("repair: a model that stops answering mid-repair is NOTRUN, never a verdict") {
    if (!have_cc()) {
        MESSAGE("NOTRUN: no gcc/clang on PATH");
        return;
    }
    prism_test::ScriptedChat llm([](const std::string&) { return std::string(kDivFix); });
    llm.fail_with("FORGE: model session lost");
    RepairCase rc("prism_repair_lost", true);
    auto rows = prism::rlef_repair(rc.fail, rc.cfg);
    REQUIRE_FALSE(rows.empty());
    for (auto& f : rows) {
        CHECK_FALSE(prism::laws::is_proof(f.status));
        CHECK_FALSE(f.extra.contains("patch_verdict"));
    }
    CHECK(rows.back().status == std::string(prism::laws::NOTRUN));
}
