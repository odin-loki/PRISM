// Doctests: the per-function pir budget (CheckOptions::function_budget_s,
// $PRISM_FUNCTION_BUDGET; docs/PIR.md "Solving"). One clock per function
// shared by both unwind attempts and the stage's re-checks; each query gets
// at most what is left; spent before every VC is answered: TIMEOUT with the
// budget message; spent after that: the verdict stands (BOUNDED stays
// BOUNDED, a validated violation stays FAILED) and a note says what was not
// tried (Law 7). Slow queries are simulated with
// CheckOptions::debug_before_query, so the tests do not depend on how fast
// the solver is.
#include <doctest/doctest.h>
#ifdef ERROR
#  undef ERROR
#endif

#include "prism/config.hpp"
#include "prism/laws.hpp"
#include "prism/pir.hpp"

#ifdef PRISM_HAS_Z3
#  include "prism/solver.hpp"
#endif

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

namespace pp = prism::pir;
namespace fs = std::filesystem;

double now_s() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

void sleep_s(double s) { std::this_thread::sleep_for(std::chrono::duration<double>(s)); }

bool contains(const std::string& s, const std::string& what) { return s.find(what) != std::string::npos; }

// The functions below, lowered by clang (-O0 + PRISM's normalisation), once.
const char* kSource = R"(/* budget test functions */
int count_up(int n) {
    int i = 0;
    while (i < n)
        i++;
    return i;
}
int sum_bounded(int n) {
    int s = 0;
    for (int i = 0; i < 100; i++)
        s += i;
    return s + (n & 1);
}
int two_loops(int n) {
    int s = 0;
    for (int i = 0; i < 100; i++)
        s += i;
    for (int j = 0; j < 100; j++)
        s += j;
    return s + (n & 1);
}
int loop_long_inv(int n) {
    int s = 0;
    for (int i = 0; i < 100; i++)
        s += 1;
    return s + (n & 1);
}
int many_props(int a, int b) {
    int m = a & 1023;
    int s = m;
    s = s + m;
    s = s + m;
    s = s + m;
    s = s + m;
    s = s + m;
    s = s + m;
    s = s + m;
    s = s + m;
    s = s + m;
    s = s + m;
    s = s + m;
    s = s + m;
    s = s + m;
    s = s + m;
    s = s + m;
    s = s + m;
    s = s + m;
    s = s + m;
    return s / b;
}
int factor(unsigned long long a, unsigned long long b) {
    int z = 1;
    if (a > 1 && b > 1 && a < 4294967296ULL && b < 4294967296ULL && a * b == 4611686014132420609ULL)
        z = 0;
    return 10 / z;
}
unsigned factor_one(unsigned long long a, unsigned long long b) {
    unsigned z = 1;
    if (a > 1 && b > 1 && a < 4294967296ULL && b < 4294967296ULL && a * b == 4611686014132420609ULL)
        z = 0;
    return 10u / z;
}
)";

struct Lowered {
    bool ok = false;
    pp::ir::Module mod;
};

const Lowered& lowered() {
    static Lowered L = [] {
        Lowered l;
        auto fe = pp::find_frontend(prism::default_config());
        if (!fe.clang || !fe.opt) return l;
        auto dir = fs::temp_directory_path() / "prism_pir_budget_src";
        fs::create_directories(dir);
        std::ofstream(dir / "b.c") << kSource;
        std::string err;
        auto ir = pp::lower_to_ir(fe, dir / "b.c", 60, err);
        if (!ir) return l;
        l.mod = pp::ir::parse_module(*ir);
        l.ok = true;
        return l;
    }();
    return L;
}

std::optional<pp::Function> fn_of(const std::string& name) {
    const auto& L = lowered();
    if (!L.ok) return std::nullopt;
    const auto* f = L.mod.find(name);
    REQUIRE(f != nullptr);
    auto tr = pp::translate(L.mod, *f);
    REQUIRE(tr.fn.has_value());
    return tr.fn;
}

pp::CheckOptions opts(double budget, int unwind = 5) {
    pp::CheckOptions o;
    o.unwind = unwind;  // <= 5: one attempt (check_function's first unwind is 4)
    o.timeout_s = 30;
    o.portfolio = false;
    o.use_cache = false;
    o.function_budget_s = budget;
    return o;
}

// Labels the hook saw, in order.
struct Seen {
    std::mutex mu;
    std::vector<std::string> labels;
    void add(const std::string& l) {
        std::lock_guard<std::mutex> g(mu);
        labels.push_back(l);
    }
    int count(const std::string& prefix) {
        std::lock_guard<std::mutex> g(mu);
        int n = 0;
        for (auto& l : labels) n += l.rfind(prefix, 0) == 0 ? 1 : 0;
        return n;
    }
};

}  // namespace

#ifdef PRISM_HAS_Z3

TEST_CASE("pir budget: a loop-free function with a spent budget is TIMEOUT; the message names the variable") {
    const char* ir = R"IR(
define i32 @g(i32 %a, i32 %b) {
entry:
  %m = and i32 %b, 7
  %d = add nsw i32 %m, 1
  %q = sdiv i32 %a, %d
  ret i32 %q
}
)IR";
    auto m = pp::ir::parse_module(ir);
    auto tr = pp::translate(m, *m.find("g"));
    REQUIRE(tr.fn.has_value());
    auto o = opts(1e-9);
    auto v = pp::check_function(*tr.fn, o);
    CHECK(v.status == prism::laws::TIMEOUT);
    CHECK(contains(v.message, "PRISM_FUNCTION_BUDGET"));
    CHECK(contains(v.message, " before "));
    CHECK(v.extra.at("function_budget_s") == "1e-09");
    // 0: no budget (the same verdict as without one)
    auto o0 = opts(0);
    auto v0 = pp::check_function(*tr.fn, o0);
    CHECK(v0.status == prism::laws::PROVED);
    CHECK_FALSE(v0.extra.contains("function_budget_s"));
}

TEST_CASE("pir budget: fractions are printed as given, not truncated") {
    const char* ir = "define i32 @f(i32 %a) {\nentry:\n  %s = add nsw i32 %a, 1\n  ret i32 %s\n}\n";
    auto m = pp::ir::parse_module(ir);
    auto tr = pp::translate(m, *m.find("f"));
    REQUIRE(tr.fn.has_value());
    auto o = opts(0.5);
    o.debug_before_query = [](const std::string&) { sleep_s(0.6); };
    auto v = pp::check_function(*tr.fn, o);
    // the one VC is answered (SAT: a + 1 overflows), the budget is spent after it
    CHECK(v.status == prism::laws::FAILED);
    CHECK(v.extra.at("function_budget_s") == "0.5");
    // a spent budget skips the nondet re-query only when there are nondet values; none here
    CHECK_FALSE(v.extra.contains("nondet_note"));
}

TEST_CASE("pir budget: both unwind attempts share one budget (skips without clang/opt)") {
    auto fn = fn_of("sum_bounded");
    if (!fn) return;
    Seen seen;
    auto o = opts(0.5, 8);  // unwind 8: first tried at unwind 4, then at 8
    o.debug_before_query = [&](const std::string& l) {
        seen.add(l);
        if (l == "unwind") sleep_s(0.6);
    };
    const double t0 = now_s();
    auto v = pp::check_function(*fn, o);
    const double took = now_s() - t0;
    // The first attempt is BOUNDED (the loop runs 100 times); the budget is
    // spent by its unwinding query, so the requested unwind gets nothing (a
    // fresh budget per attempt would ask the unwinding query at 8 as well).
    CHECK(seen.count("unwind") == 1);
    CHECK(v.status == prism::laws::BOUNDED);
    CHECK(v.extra.at("unwind") == "4");
    CHECK(v.extra.at("unwind_requested") == "8");
    CHECK(contains(v.extra.at("unwind_requested_note"), "PRISM_FUNCTION_BUDGET"));
    CHECK(contains(v.extra.at("k_induction"), "not attempted (function budget of 0.5 s spent"));
    CHECK(v.extra.at("function_budget_s") == "0.5");
    CHECK(v.extra.at("unwind_closed") == "false");
    CHECK(took < 1.1);  // one delay, not two
}

TEST_CASE("pir budget: BOUNDED stays BOUNDED when the budget is spent after every VC (single loop) (skips without "
          "clang/opt)") {
    auto fn = fn_of("sum_bounded");
    if (!fn) return;
    Seen seen;
    auto o = opts(0.3);
    o.debug_before_query = [&](const std::string& l) {
        seen.add(l);
        if (l == "unwind") sleep_s(0.4);
    };
    auto v = pp::check_function(*fn, o);
    CHECK(v.status == prism::laws::BOUNDED);
    CHECK(v.extra.at("unwind_closed") == "false");
    CHECK(v.extra.at("k_induction") == "not attempted (function budget of 0.3 s spent; PRISM_FUNCTION_BUDGET)");
    CHECK(v.extra.at("function_budget_s") == "0.3");
    CHECK(seen.count("k-induction@") == 0);
    CHECK(seen.count("houdini") == 0);
    // without the delay the same function is BOUNDED with k-induction and Houdini tried
    auto v2 = pp::check_function(*fn, opts(0));
    CHECK(v2.status == prism::laws::BOUNDED);
    CHECK(v2.extra.at("k_induction") == "step-open");
}

TEST_CASE("pir budget: BOUNDED stays BOUNDED when the budget is spent after every VC (two loops) (skips without "
          "clang/opt)") {
    auto fn = fn_of("two_loops");
    if (!fn) return;
    Seen seen;
    auto o = opts(0.3);
    o.debug_before_query = [&](const std::string& l) {
        seen.add(l);
        if (l == "unwind") sleep_s(0.4);
    };
    auto v = pp::check_function(*fn, o);
    CHECK(v.status == prism::laws::BOUNDED);
    CHECK(v.extra.at("k_induction") == "multiple-loops");
    CHECK(v.extra.at("invariants_note") == "not attempted (function budget of 0.3 s spent; PRISM_FUNCTION_BUDGET)");
    CHECK(seen.count("houdini") == 0);
}

TEST_CASE("pir budget: the k-induction step is budgeted; k=2 is not tried once it is spent (skips without "
          "clang/opt)") {
    auto fn = fn_of("sum_bounded");
    if (!fn) return;
    Seen seen;
    auto o = opts(0.5);
    o.debug_before_query = [&](const std::string& l) {
        seen.add(l);
        if (l == "k-induction@1") sleep_s(0.6);
    };
    const double t0 = now_s();
    auto v = pp::check_function(*fn, o);
    const double took = now_s() - t0;
    CHECK(v.status == prism::laws::BOUNDED);
    CHECK(seen.count("k-induction@1") == 1);
    CHECK(seen.count("k-induction@2") == 0);
    CHECK(seen.count("houdini") == 0);
    const auto& k = v.extra.at("k_induction");
    CHECK(contains(k, "PRISM_FUNCTION_BUDGET"));
    // the step at k=1 is open (s is arbitrary in it) or cut short by the budget
    CHECK((k == "step-open at k=1; k=2 not attempted (function budget of 0.5 s spent; PRISM_FUNCTION_BUDGET)" ||
           contains(k, "unknown (pir function budget of 0.5 s spent")));
    CHECK(took < 1.1);
    // the same function closes nothing without a budget either (Law 2: never promoted)
    CHECK(pp::check_function(*fn, opts(0)).status == prism::laws::BOUNDED);
}

TEST_CASE("pir budget: Houdini never runs past what is left of the function budget (skips without clang/opt)") {
    auto fn = fn_of("loop_long_inv");
    if (!fn) return;
    // unbudgeted: Houdini proves it
    auto free_run = pp::check_function(*fn, opts(0));
    REQUIRE(free_run.status == prism::laws::PROVED_UNBOUNDED);
    auto o = opts(20);
    o.timeout_s = 30;  // the search's own limit (6 x 30 s) and query floors are far above what is left
    o.function_budget = std::make_shared<pp::FunctionBudget>(20.0);
    auto b = o.function_budget;
    o.debug_before_query = [b](const std::string& l) {
        if (l == "houdini") sleep_s(std::max(0.0, b->left() - 0.05));  // 50 ms left for the search
    };
    auto v = pp::check_function(*fn, o);
    REQUIRE(v.extra.contains("houdini_seconds"));
    CHECK(std::stod(v.extra.at("houdini_seconds")) < 0.5);
    CHECK((v.status == prism::laws::BOUNDED || v.status == prism::laws::PROVED_UNBOUNDED));
    if (v.status == prism::laws::BOUNDED) {
        CHECK(contains(v.extra.at("invariants_note"), "PRISM_FUNCTION_BUDGET"));
        CHECK(v.extra.at("unwind_closed") == "false");
    }
    CHECK(b->left() > -0.5);
}

TEST_CASE("pir budget: a validated violation found by the group search is FAILED, not TIMEOUT (skips without "
          "clang/opt)") {
    auto fn = fn_of("many_props");
    if (!fn) return;
    auto free_run = pp::check_function(*fn, opts(0));
    REQUIRE(free_run.status == prism::laws::FAILED);
    REQUIRE(std::stoi(free_run.extra.at("properties")) > 16);
    const std::string cls = free_run.cls, prop = free_run.prop;
    // the violated property's own query is slow: the budget is spent before
    // the properties before it are asked as a group
    Seen seen;
    auto o = opts(0.5);
    o.debug_before_query = [&](const std::string& l) {
        seen.add(l);
        if (l.rfind(prop + "@", 0) == 0 || l == prop) sleep_s(0.6);
    };
    auto v = pp::check_function(*fn, o);
    CHECK(v.status == prism::laws::FAILED);
    CHECK(v.cls == cls);
    CHECK(v.prop == prop);
    CHECK(contains(v.extra.at("earlier_properties"), "function budget spent"));
    CHECK(v.extra.at("function_budget_s") == "0.5");
    CHECK(seen.count("properties[") >= 1);
}

TEST_CASE("pir budget: a VC whose answer the budget cut short is TIMEOUT with the budget message") {
    auto fn = fn_of("factor");
    if (!fn) return;
    auto o = opts(1.5);
    const double t0 = now_s();
    auto v = pp::check_function(*fn, o);
    const double took = now_s() - t0;
    // factoring a 62-bit semiprime by bit-blasting takes far longer than 1.5 s
    CHECK(v.status == prism::laws::TIMEOUT);
    // div0 is cut short; the next VC then finds the budget spent
    CHECK(contains(v.message, "pir function budget of 1.5 s spent "));
    CHECK(contains(v.message, "(PRISM_FUNCTION_BUDGET)"));
    CHECK(v.extra.at("function_budget_s") == "1.5");
    CHECK_FALSE(v.extra.at("function_budget_capped").empty());
    CHECK(contains(v.message, v.extra.at("function_budget_capped")));
    CHECK(took < 1.5 + 4.0);  // the budget plus the solver's stop grace
    // the function's only property query cut short: TIMEOUT "during" it,
    // with the solver's own answer kept
    auto one = fn_of("factor_one");
    REQUIRE(one);
    auto w = pp::check_function(*one, opts(1.5));
    CHECK(w.status == prism::laws::TIMEOUT);
    CHECK(contains(w.message, "pir function budget of 1.5 s spent during div0@"));
    CHECK(contains(w.extra.at("solver_answer"), "solver"));
}

TEST_CASE("pir budget: certification is not charged to the function budget") {
    const char* ir = R"IR(
define i32 @g(i32 %a, i32 %b) {
entry:
  %m = and i32 %b, 7
  %d = add nsw i32 %m, 1
  %q = sdiv i32 %a, %d
  ret i32 %q
}
)IR";
    auto m = pp::ir::parse_module(ir);
    auto tr = pp::translate(m, *m.find("g"));
    REQUIRE(tr.fn.has_value());
    auto o = opts(0.5, 8);
    o.certified = true;
    // the plain combined query spends the budget; its answer (UNSAT) stands
    o.debug_before_query = [](const std::string& l) {
        if (l.rfind("all[", 0) == 0) sleep_s(0.6);
    };
    auto v = pp::check_function(*tr.fn, o);
    prism::solver::SolveOptions so;
    const bool chain = prism::solver::find_tool("cadical", so) && prism::solver::find_tool("cake_lpr", so);
    if (chain) {
        CHECK(v.status == prism::laws::PROVED_CERTIFIED);
        CHECK(v.extra.at("certificate") == "checked");
    } else {
        CHECK(v.status == prism::laws::PROVED);
        CHECK(contains(v.extra.at("certify_note"), "not certified"));
        CHECK_FALSE(contains(v.extra.at("certify_note"), "PRISM_FUNCTION_BUDGET"));
    }
}

TEST_CASE("pir budget: $PRISM_FUNCTION_BUDGET in the pir stage (skips without clang/opt)") {
    auto fe = pp::find_frontend(prism::default_config());
    if (!fe.clang || !fe.opt) return;
    auto dir = fs::temp_directory_path() / "prism_pir_budget_stage";
    fs::remove_all(dir);
    fs::create_directories(dir);
    const fs::path loops = fs::path(__FILE__).parent_path().parent_path() / "pir" / "loops.c";
    fs::copy_file(loops, dir / "loops.c");
    auto run = [&](const char* env) {
        if (env) ::setenv("PRISM_FUNCTION_BUDGET", env, 1);
        else ::unsetenv("PRISM_FUNCTION_BUDGET");
        auto cfg = prism::default_config();
        cfg.root = dir;
        cfg.jobs = 1;
        cfg.solver_cache = dir / "cache";
        std::map<std::string, prism::Finding> got;
        for (auto& f : pp::run_pir({dir / "loops.c"}, cfg))
            if (f.function) got[*f.function] = f;
        ::unsetenv("PRISM_FUNCTION_BUDGET");
        return got;
    };
    auto plain = run(nullptr);
    REQUIRE(plain.size() >= 4);
    // (a) a spent budget: every checked row is TIMEOUT (nothing answered) or
    // keeps an answer it had (BOUNDED or FAILED), and says so
    auto spent = run("0.000001");
    for (auto& [name, f] : spent) {
        CAPTURE(name);
        if (!plain[name].extra.contains("pir_hash")) continue;  // not checked (unencoded)
        CHECK((f.status == prism::laws::TIMEOUT || f.status == prism::laws::BOUNDED ||
               f.status == prism::laws::FAILED));
        CHECK(f.extra["function_budget_s"] == "1e-06");
        if (f.status == prism::laws::TIMEOUT) CHECK(contains(f.message, "PRISM_FUNCTION_BUDGET"));
        CHECK_FALSE(f.extra.contains("env_rejected"));
    }
    // (b) 0: no budget, the same verdicts as unset
    auto zero = run("0");
    for (auto& [name, f] : plain) {
        CAPTURE(name);
        CHECK(zero[name].status == f.status);
        CHECK_FALSE(zero[name].extra.contains("function_budget_s"));
    }
    // (c) a value that is not a number of seconds is not used, and every
    // checked row says it was rejected (Law 7: never silently unbudgeted)
    auto bad = run("abc");
    for (auto& [name, f] : plain) {
        CAPTURE(name);
        CHECK(bad[name].status == f.status);
        if (!f.extra.contains("pir_hash")) continue;
        CHECK(bad[name].extra["env_rejected"] ==
              "PRISM_FUNCTION_BUDGET='abc' is not a number of seconds >= 0; ignored");
    }
    auto neg = run("-1");
    for (auto& [name, f] : neg)
        if (f.extra.contains("pir_hash")) CHECK(contains(f.extra["env_rejected"], "PRISM_FUNCTION_BUDGET='-1'"));
}

TEST_CASE("pir budget: the memory policy's re-checks of main share the function budget and the cache (skips "
          "without clang++/opt)") {
    auto fe = pp::find_frontend(prism::default_config());
    if (!fe.clangxx || !fe.opt) return;
    auto dir = fs::temp_directory_path() / "prism_pir_budget_static_init";
    fs::remove_all(dir);
    fs::create_directories(dir);
    const fs::path pir = fs::path(__FILE__).parent_path().parent_path() / "pir";
    fs::copy_file(pir / "static_init_value.cpp", dir / "value.cpp");
    auto run = [&](const char* file, const char* env, const std::string& cache) {
        if (env) ::setenv("PRISM_FUNCTION_BUDGET", env, 1);
        auto cfg = prism::default_config();
        cfg.root = dir;
        cfg.jobs = 1;
        cfg.solver_cache = dir / cache;
        std::map<std::string, prism::Finding> got;
        for (auto& f : pp::run_pir({dir / file}, cfg))
            if (f.function) got[*f.function] = f;
        ::unsetenv("PRISM_FUNCTION_BUDGET");
        return got;
    };
    // main's re-checks (the static-initialisation code; arbitrary globals)
    // use the run's solver cache: the second run answers them from it
    auto a = run("value.cpp", nullptr, "c1");
    REQUIRE(a.count("main") == 1);
    CHECK(a["main"].status == prism::laws::NEEDS_HARNESS);
    const auto& log1 = a["main"].extra["policy_rechecks"];
    CHECK(contains(log1, "static init "));
    CHECK(contains(log1, "arbitrary globals: FAILED"));
    auto b = run("value.cpp", nullptr, "c1");
    CHECK(b["main"].status == prism::laws::NEEDS_HARNESS);
    const auto& log2 = b["main"].extra["policy_rechecks"];
    CHECK(contains(log2, "(cache)"));
    CHECK_FALSE(contains(log2, "cache hits 0"));
    // a spent budget: main is TIMEOUT (nothing answered), not re-checked
    // without a budget into a verdict
    auto c = run("value.cpp", "0.000001", "c2");
    REQUIRE(c.count("main") == 1);
    CHECK(c["main"].status == prism::laws::TIMEOUT);
    CHECK(contains(c["main"].message, "PRISM_FUNCTION_BUDGET"));
    CHECK_FALSE(c["main"].extra.contains("globals"));
    CHECK_FALSE(c["main"].extra.contains("policy_rechecks"));
}

#endif
