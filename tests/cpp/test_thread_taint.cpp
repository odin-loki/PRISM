// Doctests for the thread stage (RACE-SHARED: a file global written from two
// or more functions without a mutex, in a file that starts threads) and the
// taint stage (TAINT-SINK: an untrusted source reaching a dangerous sink).
// Both are FINDS lints: they report defects, never CLEAN or a proof.

#include <doctest/doctest.h>
#ifdef ERROR
#  undef ERROR
#endif

#include "prism/cparse.hpp"
#include "prism/laws.hpp"
#include "prism/stages.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

namespace {

std::filesystem::path td() {
    return std::filesystem::path(PRISM_SOURCE_DIR) / "testdata";
}

std::vector<prism::Finding> with_cls(const std::vector<prism::Finding>& hits, const std::string& cls) {
    std::vector<prism::Finding> out;
    for (auto& f : hits)
        if (f.cls == cls) out.push_back(f);
    return out;
}

std::vector<std::string> writers_of(const prism::Finding& f) {
    auto it = f.extra.find("writers");
    REQUIRE(it != f.extra.end());
    auto j = nlohmann::json::parse(it->second);
    REQUIRE(j.is_array());
    return j.get<std::vector<std::string>>();
}

bool contains(const std::vector<std::string>& v, const std::string& s) {
    return std::find(v.begin(), v.end(), s) != v.end();
}

// The cwd for one scope (a relative fn.file resolves against it).
struct ScopedCwd {
    std::filesystem::path prev = std::filesystem::current_path();
    explicit ScopedCwd(const std::filesystem::path& to) { std::filesystem::current_path(to); }
    ~ScopedCwd() {
        std::error_code ec;
        std::filesystem::current_path(prev, ec);
    }
};

// With absolute=false fn.file is the bare file name, as a scan from the repo
// root records it; the cwd is the repo root for that call, so the result does
// not depend on where prism_tests is run from.
std::vector<prism::Finding> thread_on(const char* stem, bool absolute = false) {
    auto p = td() / stem;
    auto fns = prism::extract_functions(p, absolute ? p.string() : std::string(stem));
    if (absolute) return prism::run_thread(fns);
    ScopedCwd cwd(std::filesystem::path(PRISM_SOURCE_DIR));
    return prism::run_thread(fns);
}

}  // namespace

TEST_CASE("thread: unsynchronised global writers are RACE-SHARED (FINDS, not a proof)") {
    auto p = td() / "race_global.c";
    auto fns = prism::extract_functions(p, p.string());
    CHECK(fns.size() >= 3);
    auto bad = with_cls(prism::run_thread(fns), "RACE-SHARED");
    REQUIRE_FALSE(bad.empty());
    CHECK(bad[0].stage == "thread");
    CHECK(bad[0].status == prism::laws::FAILED);
    CHECK(bad[0].strength == prism::laws::STRENGTH_FINDS);
    CHECK_FALSE(prism::laws::is_proof(bad[0].status));
    auto w = writers_of(bad[0]);
    CHECK(contains(w, "t1"));
    CHECK(contains(w, "t2"));
}

TEST_CASE("thread: a relative file path still finds the race; no thread API is silent") {
    CHECK_FALSE(with_cls(thread_on("race_global.c"), "RACE-SHARED").empty());
    CHECK(thread_on("taint_sink.c", true).empty());
}

TEST_CASE("thread: a threaded file whose source cannot be found is NOTRUN, not silent") {
    auto p = td() / "race_global.c";
    auto fns = prism::extract_functions(p, p.string());
    REQUIRE(fns.size() >= 3);
    for (auto& fn : fns) fn.file = "no/such/dir/prism_missing_race.c";
    auto hits = prism::run_thread(fns);
    std::vector<prism::Finding> notrun;
    for (auto& f : hits)
        if (f.status == prism::laws::NOTRUN) notrun.push_back(f);
    REQUIRE(notrun.size() == 1);
    CHECK(notrun[0].stage == "thread");
    CHECK(notrun[0].file == "no/such/dir/prism_missing_race.c");
    CHECK(notrun[0].message.find("not checked") != std::string::npos);
    for (auto& f : hits) {
        CHECK(f.status != prism::laws::CLEAN);
        CHECK_FALSE(prism::laws::is_proof(f.status));
    }
    // The same functions with their real file find the race and no NOTRUN.
    auto ok = prism::run_thread(prism::extract_functions(p, p.string()));
    CHECK_FALSE(with_cls(ok, "RACE-SHARED").empty());
    CHECK(std::none_of(ok.begin(), ok.end(), [](auto& f) { return f.status == prism::laws::NOTRUN; }));
}

TEST_CASE("thread: never CLEAN or PROVED") {
    for (auto& f : thread_on("race_global.c", true)) {
        CHECK(f.status != prism::laws::CLEAN);
        CHECK(f.status != prism::laws::PROVED);
    }
}

TEST_CASE("thread: ISO C thrd_create writers race") {
    auto bad = with_cls(thread_on("iso_thread_race.c"), "RACE-SHARED");
    REQUIRE_FALSE(bad.empty());
    CHECK(bad[0].strength == prism::laws::STRENGTH_FINDS);
    CHECK(bad[0].strength != prism::laws::STRENGTH_PROVES);
    CHECK_FALSE(prism::laws::is_proof(bad[0].status));
    auto w = writers_of(bad[0]);
    CHECK(contains(w, "iso_thrd_t1"));
    CHECK(contains(w, "iso_thrd_t2"));
    CHECK(w.size() >= 2);
}

TEST_CASE("thread: a single unsynchronised writer is not a race") {
    CHECK(with_cls(thread_on("iso_thread_one_writer.c"), "RACE-SHARED").empty());
    auto p = td() / "iso_thread_race.c";
    std::vector<prism::FunctionInfo> one;
    for (auto& fn : prism::extract_functions(p, p.string()))
        if (fn.name != "iso_thrd_t2") one.push_back(fn);
    CHECK(std::any_of(one.begin(), one.end(), [](auto& fn) { return fn.name == "iso_thrd_t1"; }));
    CHECK(with_cls(prism::run_thread(one), "RACE-SHARED").empty());
}

TEST_CASE("thread: mtx_lock writers are not a race") {
    auto p = td() / "iso_thread_one_writer.c";
    auto fns = prism::extract_functions(p, p.string());
    std::vector<std::string> names;
    for (auto& fn : fns) names.push_back(fn.name);
    CHECK(contains(names, "iso_thrd_locked_a"));
    CHECK(contains(names, "iso_thrd_locked_b"));
    CHECK(with_cls(prism::run_thread(fns), "RACE-SHARED").empty());
}

TEST_CASE("thread: mtx_timedlock writers are not a race") {
    auto p = td() / "iso_thread_timedlock.c";
    auto fns = prism::extract_functions(p, p.string());
    std::vector<std::string> names;
    bool creates = false;
    for (auto& fn : fns) {
        names.push_back(fn.name);
        if (fn.body.find("thrd_create") != std::string::npos) creates = true;
    }
    CHECK(contains(names, "iso_timed_t1"));
    CHECK(contains(names, "iso_timed_t2"));
    CHECK(creates);
    CHECK(with_cls(prism::run_thread(fns), "RACE-SHARED").empty());
}

TEST_CASE("thread: std::jthread unsynchronised writers are RACE-SHARED") {
    auto hits = with_cls(thread_on("jthread_race.cpp"), "RACE-SHARED");
    REQUIRE_FALSE(hits.empty());
    CHECK(hits[0].strength == prism::laws::STRENGTH_FINDS);
    CHECK_FALSE(prism::laws::is_proof(hits[0].status));
    auto w = writers_of(hits[0]);
    CHECK(contains(w, "jrace_t1"));
    CHECK(contains(w, "jrace_t2"));
    CHECK(with_cls(thread_on("jthread_lint.cpp"), "RACE-SHARED").empty());
}

TEST_CASE("thread: FreeBSD thr_* and joined thrd are not an ISO thrd race") {
    for (auto* stem : {"thr_api.c", "thr_unenc.c", "thrd_unenc.c", "thrd_join_api.c"}) {
        INFO(stem);
        CHECK(with_cls(thread_on(stem), "RACE-SHARED").empty());
    }
}

TEST_CASE("taint: getenv reaching system is TAINT-SINK in run") {
    auto p = td() / "taint_sink.c";
    auto bad = with_cls(prism::run_taint(prism::extract_functions(p, "taint_sink.c")), "TAINT-SINK");
    REQUIRE_FALSE(bad.empty());
    CHECK(std::any_of(bad.begin(), bad.end(), [](auto& f) { return f.function == "run"; }));
    CHECK(bad[0].stage == "taint");
    CHECK(bad[0].status == prism::laws::FAILED);
    CHECK(bad[0].strength == prism::laws::STRENGTH_FINDS);
}

TEST_CASE("taint: system of a literal is not a finding") {
    auto p = td() / "taint_sink.c";
    for (auto& f : prism::run_taint(prism::extract_functions(p, "taint_sink.c")))
        CHECK(f.function != "ok");
}

TEST_CASE("taint: never CLEAN or PROVED") {
    auto p = td() / "taint_sink.c";
    for (auto& f : prism::run_taint(prism::extract_functions(p, "taint_sink.c"))) {
        CHECK(f.status != prism::laws::CLEAN);
        CHECK(f.status != prism::laws::PROVED);
    }
}
