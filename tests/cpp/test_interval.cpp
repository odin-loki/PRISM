// Doctests for the interval stage (src/prism/interval.cpp): an
// over-approximating FAILED on the planted bugs, silence (never PROVED or
// CLEAN) on correct code and on syntax the domain does not model, and LP64
// widths for long / long long / size_t.

#include <doctest/doctest.h>

#include "prism/cparse.hpp"
#include "prism/laws.hpp"
#include "prism/stages.hpp"

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;
using prism::Finding;
using prism::FunctionInfo;

fs::path repo() { return fs::path(__FILE__).parent_path().parent_path().parent_path(); }

FunctionInfo in_file(const fs::path& p, const std::string& name) {
    for (auto& f : prism::extract_functions(p, p.filename().string()))
        if (f.name == name) return f;
    FAIL("missing function " << name << " in " << p);
    return {};
}

// A testdata/*.c function by name.
FunctionInfo fn(const std::string& name) {
    static std::map<std::string, FunctionInfo> all = [] {
        std::map<std::string, FunctionInfo> m;
        for (auto& e : fs::directory_iterator(repo() / "testdata"))
            if (e.path().extension() == ".c")
                for (auto& f : prism::extract_functions(e.path(), e.path().filename().string()))
                    m.emplace(f.name, f);
        return m;
    }();
    auto it = all.find(name);
    REQUIRE_MESSAGE(it != all.end(), name);
    return it->second;
}

FunctionInfo synth(const std::string& body, std::vector<std::pair<std::string, std::string>> params,
                   std::string ret = "int") {
    FunctionInfo f;
    f.file = "synthetic.c";
    f.name = "f";
    f.kind = "SCALAR";
    f.line = 1;
    f.return_type = ret;
    f.signature = ret + " f(";
    for (std::size_t i = 0; i < params.size(); ++i)
        f.signature += (i ? ", " : "") + params[i].first + " " + params[i].second;
    f.signature += ")";
    f.params = std::move(params);
    f.body = body;
    return f;
}

// interval_function: the finding, or nullopt when the stage stays silent.
std::optional<Finding> interval(const FunctionInfo& f) {
    auto recs = prism::run_interval({f});
    REQUIRE(recs.size() <= 1);
    if (recs.empty()) return std::nullopt;
    return recs[0];
}

void check_failed(const FunctionInfo& f, const std::string& cls) {
    auto rec = interval(f);
    REQUIRE_MESSAGE(rec, f.name);
    CHECK(rec->status == prism::laws::FAILED);
    CHECK(rec->cls == cls);
    CHECK(rec->status != prism::laws::PROVED);
    CHECK_FALSE(prism::laws::is_proof(rec->status));
}

void check_silent(const FunctionInfo& f) {
    auto rec = interval(f);
    CHECK_MESSAGE(!rec, f.name, ": ", rec ? rec->cls + " " + rec->message : std::string{});
}

}  // namespace

TEST_CASE("interval: planted bugs are FAILED (over-approximate), never PROVED") {
    check_failed(fn("add_overflow"), "INT-SIGNED-OVF");
    check_failed(fn("div_param"), "INT-DIV-ZERO");
    check_failed(fn("shift_ub"), "INT-SHIFT-UB");
    check_failed(fn("abs_ter"), "INT-SIGNED-OVF");
    check_failed(fn("do_overflow"), "INT-SIGNED-OVF");
    check_failed(fn("comma_ovf"), "INT-SIGNED-OVF");
    check_failed(fn("mod_param"), "INT-DIV-ZERO");
    check_failed(fn("intmin_div"), "INT-SIGNED-OVF");
    check_failed(fn("shift_wide"), "INT-SHIFT-UB");
}

TEST_CASE("interval: correct code is silent, not PROVED") {
    for (auto* name : {"abs_ok", "saturate", "add_u", "trunc_ok", "copy_bad", "asm_vol", "wrap_u_local",
                       "wrap_u_branch", "enum_const_ok", "alignas_ok", "compound_ok"})
        check_silent(fn(name));
}

TEST_CASE("interval: pointer and unmodelled syntax are skipped, not PROVED") {
    for (auto* name : {"null_branch", "arr_esc_bad", "arr_esc_plus0", "arr_esc_plusi", "arr_esc_plus_rhs",
                       "atom_qual_bad", "const_local_bad", "struct_local_bad", "typedef_local_bad",
                       "arr_esc_paren", "esc_paren_addr", "register_local_bad", "auto_type_bad",
                       "auto_type_gnu_bad", "static_local_bad", "extern_local_bad", "anon_enum_bad",
                       "alignas_bad", "compound_bad", "const_for_bad", "anon_struct_bad"})
        check_silent(fn(name));
    auto td = repo() / "testdata";
    check_silent(in_file(td / "throw_dtor.cpp", "throws_not_dtor"));
    check_silent(in_file(td / "try_catch.cpp", "try_ok"));
}

TEST_CASE("interval: run_interval never reports PROVED, BOUNDED or CLEAN") {
    auto recs = prism::run_interval({fn("add_overflow"), fn("abs_ok"), fn("saturate")});
    bool ovf = false;
    for (auto& r : recs) {
        ovf = ovf || r.cls == "INT-SIGNED-OVF";
        CHECK(r.status != prism::laws::PROVED);
        CHECK(r.status != prism::laws::CLEAN);
    }
    CHECK(ovf);
    auto ops = prism::run_interval(
        {fn("mod_param"), fn("intmin_div"), fn("shift_wide"), fn("wrap_u_local"), fn("wrap_u_branch")});
    std::map<std::string, int> cls;
    for (auto& r : ops) {
        ++cls[r.cls];
        CHECK(r.status != prism::laws::PROVED);
        CHECK(r.status != prism::laws::BOUNDED);
        CHECK(r.status != prism::laws::CLEAN);
    }
    CHECK(cls["INT-DIV-ZERO"] > 0);
    CHECK(cls["INT-SIGNED-OVF"] > 0);
    CHECK(cls["INT-SHIFT-UB"] > 0);
}

TEST_CASE("interval: a character literal is its exact value") {
    // x / (c - 65) divides by exactly 0 when 'A' is 65, and by -1 when the
    // twin subtracts 66
    check_failed(synth("int c = 'A'; return x / (c - 65);", {{"int", "x"}}), "INT-DIV-ZERO");
    check_silent(synth("int c = 'A'; return 7 / (c - 66);", {{"int", "x"}}));
}

TEST_CASE("interval: shifts by 31 of 1 are undefined for int, not for long long") {
    auto rec = interval(synth("return 1 << n;", {{"int", "n"}}));
    REQUIRE(rec);
    CHECK(rec->cls == "INT-SHIFT-UB");
    check_silent(synth("if (n < 0) return 0; if (n > 40) return 0; long long v = 1LL << n; return v > 0;",
                       {{"int", "n"}}));
}

TEST_CASE("interval LP64: long arithmetic is 64-bit") {
    // no 32-bit overflow on a long local or a (long long) cast
    check_silent(synth("long x = a; x = x * 100000; return x > 0;", {{"int", "a"}}));
    check_silent(synth("long long p = (long long)a * b; return p > 0;", {{"int", "a"}, {"int", "b"}}));
    check_silent(synth("long v = 4294967296; return v > 0;", {}));
    check_silent(synth("unsigned long long m = 10ULL; return m > 1;", {}));
    // sizeof(long) is 8: the 4-byte branch is dead
    check_silent(synth("if (sizeof(long) == 4) return 100 / d; return 0;", {{"int", "d"}}));
    // the twins overflow at LONG_MAX, and a long value stored into an int is
    // the whole int range
    check_failed(synth("return a + 1;", {{"long", "a"}}, "long"), "INT-SIGNED-OVF");
    check_failed(synth("long long s = a * a; return s > 0;", {{"long long", "a"}}), "INT-SIGNED-OVF");
    check_failed(synth("int y = a; return y + 1;", {{"long", "a"}}), "INT-SIGNED-OVF");
    // an int stays 32-bit
    check_failed(synth("int y = a; return y * 100000;", {{"int", "a"}}), "INT-SIGNED-OVF");
}

TEST_CASE("interval LP64: testdata_fp/long_arith.c is silent, its testdata_tp twins are FAILED") {
    auto fp = repo() / "testdata_fp" / "long_arith.c";
    auto fns = prism::extract_functions(fp, fp.filename().string());
    CHECK(fns.size() == 4);
    for (auto& r : prism::run_interval(fns))
        CHECK_MESSAGE(r.status != prism::laws::FAILED, r.function.value_or(""), " ", r.message);
    auto tp = repo() / "testdata_tp" / "long_arith.c";
    for (auto* name : {"next_long", "square_ll", "ull_sum"})
        check_failed(in_file(tp, name), "INT-SIGNED-OVF");
}

TEST_CASE("interval: `int x =` with an empty initialiser reads as 0; huge literals are not a crash") {
    // x = 0, so x + 1 is never 0; with no `=` x is the full range
    check_silent(synth("int x = ; return 5 / (x + 1);", {}));
    check_failed(synth("int x; if (x > 5) return 0; if (x < -5) return 0; return 5 / (x + 1);", {}), "INT-DIV-ZERO");
    // a literal above LLONG_MAX is outside the domain: silent, not an exception
    check_silent(synth("unsigned long long m = 0xFFFFFFFFFFFFFFFF; return 0;", {}));
    check_silent(synth("return 99999999999999999999 > 0;", {}));
}
