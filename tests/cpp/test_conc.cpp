// Doctests for the conc stage (roadmap 2.6, docs/CONCURRENCY.md).
// The corpus lives in tests/conc/*.c; each file's first comment says
// TRUE (no violation within the bound: BOUNDED) or FALSE (FAILED).

#include <doctest/doctest.h>

#include "prism/conc.hpp"
#include "prism/config.hpp"
#include "prism/laws.hpp"
#include "prism/pir.hpp"

#include <filesystem>
#include <string>

namespace {

std::filesystem::path conc_dir() {
    return std::filesystem::path(__FILE__).parent_path().parent_path() / "conc";
}

bool have_frontend() {
    prism::Config cfg;
    auto fe = prism::pir::find_frontend(cfg);
    return fe.clang && fe.opt && prism::pir::z3_available();
}

prism::conc::Result run_main(const char* file, prism::conc::Options opt = {}) {
    prism::Config cfg;
    cfg.timeout = 60;
    auto all = prism::conc::check_file(conc_dir() / file, cfg, opt);
    auto it = all.find("main");
    REQUIRE_MESSAGE(it != all.end(), file);
    return it->second;
}

bool has_cls(const prism::conc::Result& r, const std::string& cls) {
    for (auto& v : r.violations)
        if (v.cls == cls) return true;
    return false;
}

}  // namespace

TEST_CASE("conc: globals of a textual module") {
    auto gs = prism::conc::parse_globals(
        "@counter = dso_local global i32 7, align 4\n"
        "@lock = dso_local global %union.pthread_mutex_t zeroinitializer, align 8\n"
        "@.str = private unnamed_addr constant [3 x i8] c\"ab\\00\", align 1\n");
    REQUIRE(gs.size() == 3);
    CHECK(gs[0].name == "counter");
    CHECK(gs[0].type == "i32");
    CHECK(gs[0].init == "7");
    CHECK(gs[1].type == "%union.pthread_mutex_t");
    CHECK(gs[2].constant);
}

TEST_CASE("conc: racy counter is a data race (FAILED with a schedule)") {
    if (!have_frontend()) return;
    auto r = run_main("racy_counter.c");
    CHECK(r.status == prism::laws::FAILED);
    REQUIRE(!r.violations.empty());
    CHECK(r.violations[0].cls == "CONC-DATA-RACE");
    CHECK(r.violations[0].msg.find("data race on counter") != std::string::npos);
    CHECK(!r.violations[0].trace.empty());
    CHECK(r.violations[0].schedule.find("T1") != std::string::npos);
    CHECK(r.violations[0].schedule.find("T2") != std::string::npos);
}

TEST_CASE("conc: mutex-protected counter is BOUNDED, never PROVED") {
    if (!have_frontend()) return;
    auto r = run_main("mutex_counter.c");
    CHECK(r.status == prism::laws::BOUNDED);
    CHECK(r.violations.empty());
    CHECK(r.status != prism::laws::PROVED);
    CHECK(r.extra.at("rounds") == "2");
    CHECK(r.extra.at("threads") == "3");
}

TEST_CASE("conc: lost update fails the assertion") {
    if (!have_frontend()) return;
    auto r = run_main("lost_update.c");
    CHECK(r.status == prism::laws::FAILED);
    CHECK(has_cls(r, "FUNC-CONTRACT"));  // the assert(counter == 2)
    CHECK(has_cls(r, "CONC-DATA-RACE"));
}

TEST_CASE("conc: opposite lock order deadlocks") {
    if (!have_frontend()) return;
    auto r = run_main("deadlock.c");
    CHECK(r.status == prism::laws::FAILED);
    REQUIRE(has_cls(r, "CONC-DEADLOCK"));
    for (auto& v : r.violations)
        if (v.cls == "CONC-DEADLOCK") CHECK(v.msg.find("pthread_mutex_lock") != std::string::npos);
}

TEST_CASE("conc: SC atomic_fetch_add is race free and BOUNDED") {
    if (!have_frontend()) return;
    auto r = run_main("atomic_counter.c");
    CHECK(r.status == prism::laws::BOUNDED);
}

TEST_CASE("conc: join then read is ordered (no race)") {
    if (!have_frontend()) return;
    auto r = run_main("join_then_read.c");
    CHECK(r.status == prism::laws::BOUNDED);
}

TEST_CASE("conc: relaxed atomics are NEEDS-HARNESS (SC only)") {
    if (!have_frontend()) return;
    auto r = run_main("relaxed.c");
    CHECK(r.status == prism::laws::NEEDS_HARNESS);
    CHECK(r.message.find("UNENCODED: memory_order_relaxed") != std::string::npos);
}

TEST_CASE("conc: one round cannot interleave; two rounds find the lost update") {
    if (!have_frontend()) return;
    prism::conc::Options one;
    one.rounds = 1;
    auto r1 = run_main("lost_update.c", one);
    CHECK(!has_cls(r1, "FUNC-CONTRACT"));  // each thread runs to completion in its only slot... or not at all
    auto r2 = run_main("lost_update.c");
    CHECK(has_cls(r2, "FUNC-CONTRACT"));
}

TEST_CASE("conc: the sequentialised program is printable") {
    if (!have_frontend()) return;
    prism::Config cfg;
    auto fe = prism::pir::find_frontend(cfg);
    std::string err;
    auto ir = prism::pir::lower_to_ir(fe, conc_dir() / "mutex_counter.c", 60, err);
    REQUIRE(ir);
    auto m = prism::pir::ir::parse_module(*ir);
    auto hs = prism::conc::harnesses(m);
    REQUIRE(hs.size() == 1);
    CHECK(hs[0] == "main");
    auto b = prism::conc::build_program(m, *ir, "main");
    REQUIRE(b.prog);
    CHECK(b.prog->threads.size() == 3);
    CHECK(b.prog->mutexes.size() == 1);
    CHECK(b.prog->vars.size() == 1);
    auto text = prism::conc::lazy_text(*b.prog);
    CHECK(text.find("seq_T1_worker") != std::string::npos);
    CHECK(text.find("pthread_mutex_lock(lock)") != std::string::npos);
    CHECK(text.find("if (") != std::string::npos);
}

TEST_CASE("conc: stage rows - no threads, no findings") {
    if (!have_frontend()) return;
    prism::Config cfg;
    cfg.root = conc_dir();
    auto none = prism::conc::run_conc({conc_dir() / "no_threads.c"}, cfg);
    CHECK(none.empty());
    auto rows = prism::conc::run_conc({conc_dir() / "racy_counter.c"}, cfg);
    REQUIRE(!rows.empty());
    for (auto& f : rows) {
        CHECK(f.stage == "conc");
        CHECK(f.status != prism::laws::PROVED);
    }
    CHECK(rows[0].status == prism::laws::FAILED);
    CHECK(!rows[0].counterexample.empty());
}
