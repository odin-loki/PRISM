// Doctests for the diff stage: two implementations of one function are built
// with gcc/clang into one harness and run on the same inputs. A missing
// compiler is NOTRUN (Law 1), agreement is CLEAN and never a proof (Law 3),
// a disagreement is FAILED, a timeout or crash is never agreement. The
// harness runs only with --allow-exec (Law 9), so each case holds a Policy.

#include <doctest/doctest.h>
#ifdef ERROR
#  undef ERROR
#endif

#include "prism/config.hpp"
#include "prism/cparse.hpp"
#include "prism/laws.hpp"
#include "prism/sandbox.hpp"
#include "prism/stages.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <stdlib.h>

namespace {

namespace fs = std::filesystem;

fs::path td() { return fs::path(PRISM_SOURCE_DIR) / "testdata"; }

std::vector<prism::FunctionInfo> pair_ab() {
    std::vector<prism::FunctionInfo> out;
    for (auto* stem : {"diff_a.c", "diff_b.c"})
        for (auto& f : prism::extract_functions(td() / stem, stem)) out.push_back(f);
    return out;
}

prism::FunctionInfo scalar(const std::string& name, const std::string& body) {
    prism::FunctionInfo f;
    f.file = "x.c";
    f.name = name;
    f.kind = "SCALAR";
    f.line = 2;
    f.signature = "int " + name + "(int x)";
    f.params = {{"int", "x"}};
    f.return_type = "int";
    f.body = body;
    f.span = {2, 5};
    return f;
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

bool has(const std::string& s, std::string_view needle) { return s.find(needle) != std::string::npos; }

bool have_cc() { return prism::Config{}.which({"gcc", "clang"}).has_value(); }

// The behavioural cases build real harnesses. Without gcc/clang they say so
// in the test output rather than passing silently (the NOTRUN path itself
// is covered by the "no gcc/clang" case, which always runs).
#define PRISM_DIFF_NEED_CC()                                                   \
    do {                                                                       \
        if (!have_cc()) {                                                      \
            MESSAGE("SKIP: no gcc/clang on PATH; this diff case asserts nothing"); \
            return;                                                            \
        }                                                                      \
    } while (0)

#ifndef _WIN32
// PATH replaced for the scope of one case (POSIX; the stage reads PATH).
struct ScopedPath {
    std::optional<std::string> prev;
    explicit ScopedPath(const std::string& value) {
        if (auto* p = std::getenv("PATH")) prev = p;
        setenv("PATH", value.c_str(), 1);
    }
    ~ScopedPath() {
        if (prev) setenv("PATH", prev->c_str(), 1);
        else unsetenv("PATH");
    }
    ScopedPath(const ScopedPath&) = delete;
    ScopedPath& operator=(const ScopedPath&) = delete;
};

fs::path empty_bin(const char* name) {
    auto d = fs::temp_directory_path() / name;
    fs::remove_all(d);
    fs::create_directories(d);
    return d;
}
#endif

}  // namespace

TEST_CASE("diff: without --allow-exec the pair is NOTRUN, never CLEAN") {
    prism::sandbox::Policy policy(false);
    auto recs = prism::run_diff(pair_ab(), td());
    REQUIRE(recs.size() == 1);
    CHECK(recs[0].status == prism::laws::NOTRUN);
    CHECK_FALSE(prism::laws::is_proof(recs[0].status));
}

#ifndef _WIN32
TEST_CASE("diff: no gcc/clang is NOTRUN with an install hint, not CLEAN") {
    prism::sandbox::Policy policy(true);
    auto bin = empty_bin("prism_diff_nocc");
    ScopedPath path(bin.string());
    auto recs = prism::run_diff(pair_ab(), td());
    REQUIRE(recs.size() == 1);
    auto& r = recs[0];
    CHECK(r.status == prism::laws::NOTRUN);
    CHECK(r.status != prism::laws::CLEAN);
    CHECK_FALSE(prism::laws::is_proof(r.status));
    CHECK(has(lower(r.message), "gcc"));
    auto it = r.extra.find("install");
    CHECK((it != r.extra.end() && !it->second.empty()));
}

TEST_CASE("diff: MSVC cl alone is not gcc/clang and stays NOTRUN") {
    prism::sandbox::Policy policy(true);
    auto bin = empty_bin("prism_diff_clonly");
    {
        std::ofstream(bin / "cl") << "#!/bin/sh\nexit 0\n";
        fs::permissions(bin / "cl", fs::perms::owner_all);
    }
    ScopedPath path(bin.string());
    auto recs = prism::run_diff(pair_ab(), td());
    REQUIRE(recs.size() == 1);
    CHECK(recs[0].status == prism::laws::NOTRUN);
    CHECK(recs[0].status != prism::laws::CLEAN);
}
#endif

TEST_CASE("diff: agreement is CLEAN, never a proof") {
    PRISM_DIFF_NEED_CC();
    prism::sandbox::Policy policy(true);
    auto recs = prism::run_diff({scalar("same_a", "    return x + 1;"), scalar("same_b", "    return 1 + x;")}, td());
    REQUIRE(recs.size() == 1);
    auto& r = recs[0];
    INFO(r.status << ": " << r.message);
    CHECK(r.stage == "diff");
    CHECK(r.status == prism::laws::CLEAN);
    CHECK(r.status != prism::laws::PROVED);
    CHECK(r.status != prism::laws::PROVED_ASSUMING);
    CHECK_FALSE(prism::laws::is_proof(r.status));
    CHECK(has(lower(r.message), "not a proof"));
}

TEST_CASE("diff: files paired by *_a/*_b stem that disagree are FAILED") {
    PRISM_DIFF_NEED_CC();
    prism::sandbox::Policy policy(true);
    auto recs = prism::run_diff(pair_ab(), td());
    REQUIRE(recs.size() == 1);
    auto& r = recs[0];
    INFO(r.status << ": " << r.message);
    CHECK(r.status == prism::laws::FAILED);
    CHECK(r.status != prism::laws::CLEAN);
    CHECK(r.status != prism::laws::PROVED);
    CHECK(has(lower(r.message), "disagree"));
    CHECK(has(r.evidence, "DIFF"));
    CHECK_FALSE(r.counterexample.empty());
    // The disagreement names the sandbox the harness ran in.
    auto it = r.extra.find("sandbox");
    REQUIRE(it != r.extra.end());
    CHECK(it->second == prism::sandbox::kind());
}

TEST_CASE("diff: name_a/name_b pairing that disagrees is FAILED") {
    PRISM_DIFF_NEED_CC();
    prism::sandbox::Policy policy(true);
    auto recs = prism::run_diff({scalar("foo_a", "    return x;"), scalar("foo_b", "    return x ^ 1;")}, td());
    REQUIRE(recs.size() == 1);
    CHECK(recs[0].status == prism::laws::FAILED);
    CHECK(recs[0].status != prism::laws::CLEAN);
    CHECK_FALSE(prism::laws::is_proof(recs[0].status));
    REQUIRE(recs[0].function.has_value());
    CHECK(has(*recs[0].function, "foo_a"));
    CHECK(has(*recs[0].function, "foo_b"));
}

TEST_CASE("diff: a `// diff:` comment pairs two functions; disagreement is FAILED") {
    PRISM_DIFF_NEED_CC();
    prism::sandbox::Policy policy(true);
    auto dir = fs::temp_directory_path() / "prism_diff_comment";
    fs::create_directories(dir);
    auto src = dir / "pair.c";
    std::ofstream(src, std::ios::binary) << "// diff: impl_b\n"
                                            "int impl_a(int x) { return x; }\n"
                                            "int impl_b(int x) { return x ^ 1; }\n";
    auto fns = prism::extract_functions(src, src.string());
    std::vector<std::string> names;
    for (auto& f : fns) names.push_back(f.name);
    std::sort(names.begin(), names.end());
    CHECK(names == std::vector<std::string>{"impl_a", "impl_b"});
    auto recs = prism::run_diff(fns, dir);
    REQUIRE(recs.size() == 1);
    CHECK(recs[0].status == prism::laws::FAILED);
    CHECK(recs[0].status != prism::laws::CLEAN);
    CHECK_FALSE(prism::laws::is_proof(recs[0].status));
    CHECK(has(lower(recs[0].message), "disagree"));
}

TEST_CASE("diff: a harness that does not compile is ERROR, not CLEAN") {
    PRISM_DIFF_NEED_CC();
    prism::sandbox::Policy policy(true);
    auto recs = prism::run_diff({scalar("bad_a", "    return x +;"), scalar("bad_b", "    return x;")}, td());
    REQUIRE(recs.size() == 1);
    CHECK(recs[0].status == prism::laws::ERROR);
    CHECK(recs[0].status != prism::laws::CLEAN);
    CHECK_FALSE(prism::laws::is_proof(recs[0].status));
    CHECK(has(recs[0].message, "diff compile:"));
    CHECK(recs[0].message.size() <= std::string("diff compile: ").size() + 400);
}

TEST_CASE("diff: a timeout is not agreement") {
    PRISM_DIFF_NEED_CC();
    prism::sandbox::Policy policy(true);
    // Only input 42 loops; every other input agrees.
    auto recs = prism::run_diff(
        {scalar("spin_a", "    if (x == 42) { for (;;) { } }\n    return x;"), scalar("spin_b", "    return x;")}, td());
    REQUIRE(recs.size() == 1);
    INFO(recs[0].status << ": " << recs[0].message);
    CHECK(recs[0].status == prism::laws::TIMEOUT);
    CHECK(recs[0].status != prism::laws::CLEAN);
    CHECK_FALSE(prism::laws::is_proof(recs[0].status));
    CHECK(has(recs[0].message, "not agreement, not a proof"));
}

TEST_CASE("diff: a crash is not agreement") {
    PRISM_DIFF_NEED_CC();
    prism::sandbox::Policy policy(true);
    auto recs = prism::run_diff({scalar("seg_a", "    if (x == 42) { volatile int *p = 0; return *p; }\n    return x;"),
                                 scalar("seg_b", "    return x;")},
                                td());
    REQUIRE(recs.size() == 1);
    INFO(recs[0].status << ": " << recs[0].message);
    CHECK(recs[0].status == prism::laws::CRASH);
    CHECK(recs[0].status != prism::laws::CLEAN);
    CHECK_FALSE(prism::laws::is_proof(recs[0].status));
}

TEST_CASE("diff: parameter types with inner whitespace keep their C size") {
    PRISM_DIFF_NEED_CC();
    prism::sandbox::Policy policy(true);
    // "unsigned   char" is one byte: each input is one byte, the harness reads
    // it into x, so the byte 0x2a reaches both functions as 42.
    auto a = scalar("uc_a", "    return x == 42 ? 1 : 0;");
    auto b = scalar("uc_b", "    return 0;");
    a.params = b.params = {{"unsigned   char", "x"}};
    auto recs = prism::run_diff({a, b}, td());
    REQUIRE(recs.size() == 1);
    INFO(recs[0].status << ": " << recs[0].message);
    CHECK(recs[0].status == prism::laws::FAILED);
    CHECK(recs[0].counterexample == "2a");
}
