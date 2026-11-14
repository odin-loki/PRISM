// Doctests: function-level parallelism in the pir stage (one task per
// function on FORGE's work-stealing scheduler, prism::pool::TaskGraph) and the
// whole-function verdict cache (src/prism/pir/fncache.hpp). The verdicts of a
// unit must not depend on --jobs, and a cache hit must give back exactly the
// verdict the check gave, marked as a hit (Law 7).
#include <doctest/doctest.h>
#ifdef ERROR
#  undef ERROR
#endif

#include "prism/config.hpp"
#include "prism/laws.hpp"
#include "prism/pir.hpp"
#include "prism/pir/frontend.hpp"

#include <nlohmann/json.hpp>

#include "../../src/prism/pir/fncache.hpp"

#include <cstdlib>
#ifdef _WIN32
#  include <stdlib.h>
#endif
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <tuple>
#include <vector>

namespace {

namespace pp = prism::pir;
namespace fs = std::filesystem;

bool skip_without_frontend(const prism::Config& cfg) {
    std::string why;
    if (!pp::frontend_usable(cfg, &why)) {
        MESSAGE(why);
        return true;
    }
    if (!pp::z3_available()) {
        MESSAGE("NOTRUN: pir needs Z3 (rebuild with -DPRISM_Z3=ON)");
        return true;
    }
    return false;
}

fs::path fresh_dir(const std::string& tag) {
    auto d = fs::temp_directory_path() / ("prism-pir-par-" + tag + "-" + std::to_string(std::rand()));
    fs::remove_all(d);
    fs::create_directories(d);
    return d;
}

// A unit with many small functions: proved ones, refuted ones, a loop.
std::string many_functions_unit() {
    std::string s = "#include <limits.h>\n";
    for (int i = 0; i < 12; ++i) {
        const auto n = std::to_string(i);
        s += "int ok" + n + "(int a) { if (a > 1000 || a < -1000) return 0; return a * " + std::to_string(i + 2) + "; }\n";
        s += "int bad" + n + "(int a) { return a + INT_MAX - " + n + "; }\n";
    }
    s += "int loop_sum(unsigned n) { int s = 0; for (unsigned i = 0; i < n && i < 4; ++i) s += (int)i; return s; }\n";
    s += "int divz(int a, int b) { return a / b; }\n";
    return s;
}

using Row = std::tuple<std::string, std::string, std::string, std::string>;  // function, status, cls, message

std::vector<Row> rows_of(const std::vector<prism::Finding>& out) {
    std::vector<Row> rows;
    for (const auto& f : out)
        if (f.function) rows.emplace_back(*f.function, f.status, f.cls, f.message);
    return rows;
}

void test_setenv(const char* key, const char* value) {
#ifdef _WIN32
    _putenv_s(key, value);
#else
    ::setenv(key, value, 1);
#endif
}

void test_unsetenv(const char* key) {
#ifdef _WIN32
    _putenv_s(key, "");
#else
    ::unsetenv(key);
#endif
}

pp::Function tiny_function() {
    pp::Function fn;
    fn.name = fn.ir_name = "f";
    fn.file = "f.c";
    fn.line = 3;
    fn.vars = {{"a", 32, false}, {"b", 32, false}};
    fn.params = {0};
    fn.ret_width = 32;
    pp::Block b;
    b.name = "entry";
    pp::Stmt st;
    st.kind = pp::Stmt::Assign;
    st.dst = 1;
    st.op = pp::Op::Add;
    st.args = {pp::Arg::v(0, 32), pp::Arg::c(32, 1)};
    b.stmts.push_back(st);
    b.term.kind = pp::Term::Ret;
    b.term.val = pp::Arg::v(1, 32);
    fn.blocks.push_back(b);
    return fn;
}

}  // namespace

TEST_CASE("pir fncache: the fingerprint sees every field, not only the display text") {
    const auto base = tiny_function();
    const auto fp = pp::fncache::fingerprint(base);
    CHECK(fp == pp::fncache::fingerprint(tiny_function()));
    std::vector<pp::Function> variants(8, base);
    variants[0].ptr = pp::kILP32;                     // not in to_text
    variants[1].assumptions = {"n <= 4"};             // not in to_text
    variants[2].line = 4;                             // messages carry the line
    variants[3].blocks[0].stmts[0].args[1].bits = 2;  // the program
    variants[4].ptr_params = {"p"};
    variants[5].uses_memory = true;
    variants[6].vars[1].fp = true;
    variants[7].loop_locs[0] = {3, 5};
    for (std::size_t i = 0; i < variants.size(); ++i) {
        INFO("variant " << i);
        CHECK(pp::fncache::fingerprint(variants[i]) != fp);
    }
}

TEST_CASE("pir fncache: only completed verdicts are storable") {
    pp::Verdict v;
    v.status = std::string(prism::laws::PROVED);
    CHECK(pp::fncache::storable(v));
    for (auto st : {prism::laws::UNKNOWN, prism::laws::TIMEOUT, prism::laws::ERROR, prism::laws::NOTRUN,
                    prism::laws::PROVED_CERTIFIED}) {
        v.status = std::string(st);
        CHECK_FALSE(pp::fncache::storable(v));
    }
    v.status = std::string(prism::laws::BOUNDED);
    CHECK(pp::fncache::storable(v));
    v.extra["function_budget_s"] = "300";  // cut by the function budget
    CHECK_FALSE(pp::fncache::storable(v));
    v.extra.clear();
    v.extra["invariants_note"] = "not attempted (Houdini budget spent)";
    CHECK_FALSE(pp::fncache::storable(v));
}

TEST_CASE("pir fncache: key follows the options; round trip marks the hit") {
    const auto dir = fresh_dir("key");
    pp::CheckOptions o;
    o.cache_dir = (dir / "cache").string();
    const auto fn = tiny_function();
    const auto k = pp::fncache::key(fn, o);
    if (k.empty()) {
        MESSAGE("engine binary not identifiable on this platform: verdict cache off");
        return;
    }
    CHECK(k.size() == 64);
    CHECK(pp::fncache::key(fn, o) == k);
    auto o2 = o;
    o2.unwind = 9;
    CHECK(pp::fncache::key(fn, o2) != k);
    o2 = o;
    o2.certified = true;  // certified runs need a fresh certificate
    CHECK(pp::fncache::key(fn, o2).empty());
    o2 = o;
    o2.only_props = {"ovf+"};
    CHECK(pp::fncache::key(fn, o2).empty());
    test_setenv("PRISM_PIR_DEEPEN_MAX", "17");
    CHECK(pp::fncache::key(fn, o) != k);  // any PRISM_* setting is part of the key
    test_unsetenv("PRISM_PIR_DEEPEN_MAX");
    CHECK(pp::fncache::key(fn, o) == k);
    test_setenv("PRISM_PIR_FN_CACHE", "0");
    CHECK(pp::fncache::key(fn, o).empty());
    test_unsetenv("PRISM_PIR_FN_CACHE");

    CHECK_FALSE(pp::fncache::get(o, k).has_value());
    pp::Verdict v;
    v.status = std::string(prism::laws::FAILED);
    v.message = "signed overflow";
    v.cls = "INT-SIGNED-OVF";
    v.prop = "ovf+";
    v.line = 3;
    v.cex = {{"a", 0x7fffffffu}};
    v.cex_args = {0x7fffffffu};
    v.extra["unwind"] = "8";
    pp::fncache::put(o, k, v);
    auto got = pp::fncache::get(o, k);
    REQUIRE(got.has_value());
    CHECK(got->status == v.status);
    CHECK(got->message == v.message);
    CHECK(got->cls == v.cls);
    CHECK(got->prop == v.prop);
    CHECK(got->line == v.line);
    CHECK(got->cex == v.cex);
    CHECK(got->cex_args == v.cex_args);
    CHECK(got->extra.at("unwind") == "8");
    CHECK(got->extra.at("verdict_cache").starts_with("hit "));
    // an incomplete verdict is never written
    pp::Verdict u;
    u.status = std::string(prism::laws::UNKNOWN);
    pp::fncache::put(o, std::string(64, 'a'), u);
    CHECK_FALSE(pp::fncache::get(o, std::string(64, 'a')).has_value());
    fs::remove_all(dir);
}

TEST_CASE("pir parallel: a unit's verdicts do not depend on --jobs; a re-run hits the verdict cache") {
    auto cfg = prism::default_config();
    if (skip_without_frontend(cfg)) return;
    const auto dir = fresh_dir("e2e");
    {
        std::ofstream(dir / "many.c") << many_functions_unit();
        std::ofstream(dir / "two.c") << "int two_ok(int a) { return a & 7; }\nint two_bad(int a) { return 10 / a; }\n";
    }
    cfg.root = dir;
    cfg.out = dir / "out";
    const std::vector<fs::path> srcs{dir / "many.c", dir / "two.c"};

    test_setenv("PRISM_PIR_FN_CACHE", "0");
    cfg.jobs = 1;
    cfg.solver_cache = dir / "cache1";
    const auto serial = rows_of(pp::run_pir(srcs, cfg));
    cfg.jobs = 4;
    cfg.solver_cache = dir / "cache4";
    const auto parallel = rows_of(pp::run_pir(srcs, cfg));
    test_unsetenv("PRISM_PIR_FN_CACHE");
    REQUIRE(serial.size() >= 28);
    CHECK(serial == parallel);  // same rows in the same order
    std::map<std::string, std::string> by_fn;
    for (const auto& [fn, st, cls, msg] : serial) by_fn[fn] = st;
    CHECK(by_fn["ok3"] == prism::laws::PROVED);
    CHECK(by_fn["bad3"] == prism::laws::FAILED);
    CHECK(by_fn["divz"] == prism::laws::FAILED);
    CHECK(by_fn["two_bad"] == prism::laws::FAILED);

    // fresh cache: misses fill it; the second run answers from it
    cfg.solver_cache = dir / "cache-fn";
    const auto first = pp::run_pir(srcs, cfg);
    const auto second = pp::run_pir(srcs, cfg);
    CHECK(rows_of(first) == serial);
    CHECK(rows_of(second) == serial);
    int hits_first = 0, hits_second = 0, checked = 0;
    for (const auto& f : first)
        if (f.extra.count("verdict_cache")) ++hits_first;
    for (const auto& f : second) {
        if (!f.function || (f.status != prism::laws::PROVED && f.status != prism::laws::FAILED)) continue;
        ++checked;
        INFO(*f.function);
        CHECK(f.extra.count("verdict_cache") == 1);  // every completed proof or refutation came from the cache
        if (f.extra.count("verdict_cache")) ++hits_second;
    }
    CHECK(hits_first == 0);
    CHECK(checked >= 26);
    CHECK(hits_second == checked);
    fs::remove_all(dir);
}

TEST_CASE("pir library keys: shape ignores names and constants, exact does not") {
    const auto a = tiny_function();
    auto b = tiny_function();
    b.name = b.ir_name = "g";
    b.file = "other.c";
    b.line = 99;
    b.vars[0].name = "x";
    b.blocks[0].name = "start";
    b.blocks[0].stmts[0].args[1].bits = 7;  // another constant, same width
    CHECK(pp::fncache::shape_key(a) == pp::fncache::shape_key(b));
    CHECK(pp::fncache::exact_key(a) != pp::fncache::exact_key(b));
    auto c = tiny_function();
    c.blocks[0].stmts[0].op = pp::Op::Sub;  // another operation: another shape
    CHECK(pp::fncache::shape_key(a) != pp::fncache::shape_key(c));
    auto d = tiny_function();
    d.vars[1].width = 64;  // another width: another shape
    CHECK(pp::fncache::shape_key(a) != pp::fncache::shape_key(d));
    const auto fj = nlohmann::json::parse(pp::fncache::features_json(a));
    CHECK(fj.at("blocks") == 1);
    CHECK(fj.at("params") == 1);
    CHECK(fj.at("back_edges") == 0);
}
