// prism::exec / prism::pool / prism::cache / prism::fail: the thin
// replaceable-backend interfaces (exec.hpp, pool.hpp, cache.hpp, exit.hpp).
#include "doctest.h"

#include "prism/cache.hpp"
#include "prism/exec.hpp"
#include "prism/exit.hpp"
#include "prism/log.hpp"
#include "prism/pool.hpp"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <string>
#include <fstream>
#include <vector>

namespace fs = std::filesystem;

namespace {
std::vector<std::string> g_seen;
void capture(prism::log::Level l, std::string_view c, std::string_view m) {
    g_seen.push_back(std::to_string(static_cast<int>(l)) + "|" + std::string(c) + "|" + std::string(m));
}
prism::exec::Result fake_run(const prism::exec::Spec& s) {
    prism::exec::Result r;
    r.exit_code = 7;
    r.out = s.argv.empty() ? "" : s.argv[0];
    return r;
}
}  // namespace

TEST_CASE("exit: fail() logs through prism::log and returns the named code") {
    g_seen.clear();
    auto old = prism::log::set_sink(&capture);
    CHECK(prism::fail("conformance", "bad --filter regex") == 2);
    CHECK(prism::fail("", "tool missing", prism::Exit::Error) == 3);
    CHECK(prism::fail_usage("", "unrecognized arguments: -x", "(prism --help lists the options)") == 2);
    prism::log::set_sink(old);
    REQUIRE(g_seen.size() == 4);
    CHECK(g_seen[0] == "3|conformance|bad --filter regex");
    CHECK(g_seen[3] == "4||(prism --help lists the options)");
}

TEST_CASE("log: program prefix, empty component and plain lines on the default sink") {
    // Formatting is exercised through a capturing sink in test_log.cpp; here
    // the program name round-trips.
    prism::log::set_program("prism-qa");
    CHECK(prism::log::program() == "prism-qa");
    prism::log::set_program("prism");
    CHECK(prism::log::cat("a", 1, 'b') == "a1b");
}

TEST_CASE("exec: the default backend runs a process; set_backend swaps it") {
#ifndef _WIN32
    prism::exec::Spec s;
    s.argv = {"sh", "-c", "printf hi; exit 4"};
    s.timeout_ms = 10000;
    auto r = prism::exec::run(s);
    CHECK_FALSE(r.failed);
    CHECK(r.exit_code == 4);
    CHECK(r.out == "hi");
    s.argv = {"sh", "-c", "sleep 5"};
    s.timeout_ms = 200;
    CHECK(prism::exec::run(s).timed_out);
#endif
    auto old = prism::exec::set_backend(&fake_run);
    prism::exec::Spec f;
    f.argv = {"anything"};
    auto r2 = prism::exec::run(f);
    CHECK(r2.exit_code == 7);
    CHECK(r2.out == "anything");
    prism::exec::set_backend(old);
}

TEST_CASE("pool: parallel_for covers [0, n) exactly once") {
    std::vector<std::atomic<int>> hits(1000);
    prism::pool::parallel_for(3, hits.size(), 7, [&](std::size_t b, std::size_t e) {
        for (auto i = b; i < e; ++i) hits[i]++;
    });
    for (auto& h : hits) CHECK(h.load() == 1);
    CHECK(prism::pool::size() >= 1);
}

TEST_CASE("cache: put/get/contains/erase/prune and corruption is a miss") {
    const fs::path dir = fs::temp_directory_path() / ("prism-cache-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::remove_all(dir);
    const auto k = prism::cache::put(dir, std::string_view("hello"));
    CHECK(k.size() == 64);
    CHECK(prism::cache::contains(dir, k));
    CHECK(prism::cache::get(dir, k).value_or("") == "hello");
    CHECK_FALSE(prism::cache::get(dir, std::string(64, '0')).has_value());
    CHECK_FALSE(prism::cache::put(dir, "not-hex", "x"));
    {
        std::ofstream(dir / k.substr(0, 2) / k.substr(2), std::ios::binary | std::ios::app) << "tamper";
    }
    CHECK_FALSE(prism::cache::get(dir, k).has_value());
    CHECK(prism::cache::stats(dir).corrupt >= 1);
    prism::cache::put(dir, std::string_view("a"));
    prism::cache::put(dir, std::string_view("b"));
    CHECK(prism::cache::prune(dir, 0) > 0);
    CHECK(prism::cache::stats(dir).entries == 0);
    fs::remove_all(dir);
}
