// Doctests for the concrete interpreter (src/prism/stages/interp.cpp) and
// the concrete fuzzer (src/prism/stages/fuse.cpp fuzz_function): the UB
// oracle on the planted corpus, LP64 widths, argument decoding, and the
// fuzzer's CRASH / CLEAN / NEEDS-HARNESS / ERROR rows.

#include <doctest/doctest.h>

#include "prism/cparse.hpp"
#include "prism/journal.hpp"
#include "prism/laws.hpp"
#include "prism/models.hpp"
#include "prism/stages.hpp"

#include "../../src/prism/stages/fuse.hpp"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;
namespace sd = prism::stages_detail;
using prism::FunctionInfo;
using sd::Args;

constexpr int64_t kIntMax = std::numeric_limits<int32_t>::max();
constexpr int64_t kIntMin = std::numeric_limits<int32_t>::min();
constexpr int64_t kI64Max = std::numeric_limits<int64_t>::max();

fs::path repo() { return fs::path(PRISM_SOURCE_DIR); }

fs::path td() { return repo() / "testdata"; }

// The testdata function `name` (C or C++ file), with the file it came from.
std::pair<FunctionInfo, fs::path> fn_named(const std::string& name) {
    static std::map<std::string, std::pair<FunctionInfo, fs::path>> all = [] {
        std::map<std::string, std::pair<FunctionInfo, fs::path>> m;
        for (auto& e : fs::directory_iterator(td())) {
            auto ext = e.path().extension();
            if (ext != ".c" && ext != ".cpp") continue;
            for (auto& f : prism::extract_functions(e.path(), e.path().string()))
                m.emplace(f.name, std::make_pair(f, e.path()));
        }
        return m;
    }();
    auto it = all.find(name);
    REQUIRE_MESSAGE(it != all.end(), name);
    return it->second;
}

FunctionInfo fn(const std::string& name) { return fn_named(name).first; }

// A synthetic SCALAR function `int f(<params>) { body }`.
FunctionInfo synth(const std::string& body, std::vector<std::pair<std::string, std::string>> params = {{"int", "x"}},
                   std::string ret = "int", std::string file = "") {
    FunctionInfo f;
    f.file = std::move(file);
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

sd::ExecResult run(const FunctionInfo& f, Args a = {}) { return sd::execute(f, a); }

// The inverse of decode_args (test helper): each parameter little-endian
// at its kCTypeSize width.
std::vector<uint8_t> pack_args(const FunctionInfo& f, const Args& args) {
    std::vector<uint8_t> raw;
    for (auto& [typ, name] : f.params) {
        if (name.empty()) continue;
        auto key = sd::ctype_key(typ);
        int sz = sd::kCTypeSize.contains(key) ? sd::kCTypeSize.at(key) : 4;
        auto it = args.find(name);
        auto v = static_cast<uint64_t>(it == args.end() ? 0 : it->second);
        for (int k = 0; k < sz; ++k) raw.push_back(static_cast<uint8_t>(v >> (8 * k)));
    }
    return raw;
}

std::string extra(const prism::Finding& f, const char* key) {
    auto it = f.extra.find(key);
    return it == f.extra.end() ? std::string{} : it->second;
}

prism::Finding fuzz(const std::string& name, double budget = 0.1, int iters = 1) {
    auto [f, p] = fn_named(name);
    return sd::fuzz_function(f, p, budget, iters, nullptr);
}

prism::Finding fuse(const std::string& name) {
    auto [f, p] = fn_named(name);
    auto recs = prism::run_fuse({f}, {}, p.parent_path(), 0.1, 1, false);
    REQUIRE_FALSE(recs.empty());
    return recs[0];
}

bool is(const prism::Finding& f, std::string_view st) { return f.status == st; }

bool f_has(const prism::Finding& f, const char* key) { return f.extra.contains(key); }

}  // namespace

// ---- concrete execute on the planted corpus

TEST_CASE("concrete: add_overflow is INT-SIGNED-OVF at INT_MAX, 100 at 0") {
    auto f = fn("add_overflow");
    CHECK(run(f, {{"x", kIntMax}}).ub == "INT-SIGNED-OVF");
    auto ok = run(f, {{"x", 0}});
    CHECK(ok.ub.empty());
    CHECK(ok.value == 100);
}

TEST_CASE("concrete: div_param divides by zero; INT_MIN / -1 overflows") {
    auto f = fn("div_param");
    CHECK(run(f, {{"x", 1}, {"y", 0}}).ub == "INT-DIV-ZERO");
    auto ok = run(f, {{"x", 8}, {"y", 2}});
    CHECK(ok.ub.empty());
    CHECK(ok.value == 4);
    CHECK(run(f, {{"x", kIntMin}, {"y", -1}}).ub == "INT-SIGNED-OVF");
}

TEST_CASE("concrete: oob_write out of bounds at 4 and -1, in bounds at 0") {
    auto f = fn("oob_write");
    CHECK(run(f, {{"i", 4}}).ub == "MEM-OOB-WRITE");
    CHECK(run(f, {{"i", -1}}).ub == "MEM-OOB-WRITE");
    auto ok = run(f, {{"i", 0}});
    CHECK(ok.ub.empty());
    CHECK(ok.value == 1);
}

TEST_CASE("concrete: shifts past the width are INT-SHIFT-UB") {
    CHECK(run(fn("shift_ub"), {{"x", 0}}).ub == "INT-SHIFT-UB");
    CHECK(run(synth("return x << 32;"), {{"x", 1}}).ub == "INT-SHIFT-UB");
}

TEST_CASE("concrete: a pointer function is skipped, not run") {
    auto rec = run(fn("null_branch"));
    CHECK(rec.ub.empty());
    CHECK(rec.error == "skip-pointer");
}

TEST_CASE("concrete: saturate has no UB on the edges") {
    auto f = fn("saturate");
    auto rec = run(f, {{"x", kIntMax}});
    CHECK(rec.ub.empty());
    CHECK(rec.error.empty());
    for (int64_t x : {int64_t{0}, int64_t{-1}, int64_t{1}, int64_t{100}, int64_t{101}, kIntMax, kIntMin})
        CHECK_MESSAGE(run(f, {{"x", x}}).ub.empty(), x);
}

TEST_CASE("concrete: unmodelled locals are named, not a trailing-token error") {
    const std::vector<std::pair<const char*, const char*>> cases = {
        {"const_local_bad", "const unencoded"},
        {"struct_local_bad", "struct unencoded"},
        {"typedef_local_bad", "typedef local unencoded"},
        {"register_local_bad", "storage-class unencoded"},
        {"auto_type_bad", "storage-class unencoded"},
        {"auto_type_gnu_bad", "storage-class unencoded"},
        {"static_local_bad", "storage-duration unencoded"},
        {"extern_local_bad", "storage-duration unencoded"},
        {"anon_enum_bad", "anon enum unencoded"},
        {"alignas_bad", "alignas unencoded"},
        {"compound_bad", "compound-lit unencoded"},
        {"const_for_bad", "const unencoded"},
        {"anon_struct_bad", "struct unencoded"},
    };
    for (auto& [name, reason] : cases) {
        auto f = fn(name);
        auto rec = run(f, {{f.params.empty() ? "_" : f.params[0].second, 1}});
        CHECK_MESSAGE(rec.ub.empty(), name);
        CHECK_MESSAGE(rec.error.find(reason) != std::string::npos, name, " ", rec.error);
    }
    auto ok = run(fn("enum_const_ok"), {{"n", 1}});
    CHECK(ok.error.empty());
    CHECK(ok.ub.empty());
}

TEST_CASE("concrete: corpus values (abs, loop, switch, ternary, comma, do)") {
    auto abs_ok = run(fn("abs_ok"), {{"x", kIntMin}});
    CHECK(abs_ok.ub.empty());
    CHECK(abs_ok.value == kIntMax);
    auto loop = run(fn("loop_prove"), {{"x", 10}});
    CHECK(loop.ub.empty());
    CHECK(loop.value == 3);
    auto sw = run(fn("fsm_step"), {{"state", 0}, {"ev", 1}});
    CHECK(sw.ub.empty());
    CHECK(sw.value == 1);
    CHECK(run(fn("abs_ter"), {{"x", kIntMin}}).ub == "INT-SIGNED-OVF");
    CHECK(run(fn("do_overflow"), {{"n", 1073741824}}).ub == "INT-SIGNED-OVF");
    auto pick = run(fn("pick"), {{"x", 1}});
    CHECK(pick.ub.empty());
    CHECK(pick.value == 1);
    CHECK(run(fn("comma_ovf"), {{"x", 1073741824}}).ub == "INT-SIGNED-OVF");
    auto cs = run(fn("comma_sum"), {{"x", 1}, {"y", 2}});
    CHECK(cs.ub.empty());
    CHECK(cs.value == 0);
}

TEST_CASE("concrete: goto is not silently run past") {
    auto rec = run(fn("with_goto"), {{"x", 0}});
    CHECK(rec.ub.empty());
    CHECK(rec.value != 1);
}

TEST_CASE("concrete: bare return and char literals parse") {
    auto bare = run(fn("taut_bound_bad"), {{"idx", 0}});
    CHECK(bare.error.empty());
    CHECK(bare.ub.empty());
    auto tr = run(fn("trunc_ok"));
    CHECK(tr.error.empty());
    CHECK(tr.ub.empty());
    CHECK(tr.value == 1 + 'A' + 42);
}

TEST_CASE("concrete: throw is an error, not a silent return") {
    auto p = td() / "throw_dtor.cpp";
    FunctionInfo hit;
    for (auto& f : prism::extract_functions(p, p.string()))
        if (f.name == "throws_not_dtor") hit = f;
    REQUIRE(hit.name == "throws_not_dtor");
    auto rec = run(hit);
    CHECK(rec.ub.empty());
    CHECK(rec.error.find("throw") != std::string::npos);
}

TEST_CASE("concrete: strcpy overflow is MEM-OOB-WRITE, snprintf is not") {
    auto bad = fn("copy_bad");
    CHECK(bad.body.find("overflow") != std::string::npos);
    auto rec = run(bad);
    CHECK(rec.error.empty());
    CHECK(rec.ub == "MEM-OOB-WRITE");
    auto ok = run(fn("copy_ok"));
    CHECK(ok.error.empty());
    CHECK(ok.ub.empty());
}

TEST_CASE("concrete: postfix decrement overflows at INT_MIN and stops at 0") {
    auto f = fn("empty_inf_ok");
    auto hit = run(f, {{"n", kIntMin}});
    CHECK(hit.error.empty());
    CHECK(hit.ub == "INT-SIGNED-OVF");
    auto ok = run(f, {{"n", 0}});
    CHECK(ok.error.empty());
    CHECK(ok.ub.empty());
    auto u = run(fn("add_u"), {{"a", kIntMax}, {"b", kIntMax}});
    CHECK(u.ub.empty());
    CHECK(u.error.empty());
}

TEST_CASE("concrete: unsigned index guard and out-of-bounds read") {
    auto ok = run(fn("idx_u_ok"), {{"i", -1}});
    CHECK(ok.ub.empty());
    CHECK(ok.value == 0);
    CHECK(run(fn("idx_u_bad"), {{"i", 4}}).ub == "MEM-OOB-READ");
}

TEST_CASE("concrete: long long overflows at 2^63, not at INT_MAX + 1") {
    auto f = fn("add_ll");
    auto ok = run(f, {{"a", 1}, {"b", kIntMax}});
    CHECK(ok.ub.empty());
    CHECK(ok.value == kIntMax + 1);
    CHECK(run(f, {{"a", int64_t{1} << 62}, {"b", int64_t{1} << 62}}).ub == "INT-SIGNED-OVF");
    CHECK(run(f, {{"a", kI64Max}, {"b", 1}}).ub == "INT-SIGNED-OVF");
}

// ---- LP64 widths, 64-bit locals and literals

TEST_CASE("concrete LP64: long and size_t are 64 bits") {
    auto mul = synth("long x = a; x = x * 100000; return x > 0;", {{"int", "a"}});
    auto r = run(mul, {{"a", kIntMax}});
    CHECK(r.ub.empty());
    CHECK(r.error.empty());
    CHECK(r.value == 1);
    auto sz = synth("if (sizeof(long) == 4) return 100 / d; return sizeof(long) + sizeof(size_t);", {{"int", "d"}});
    auto s = run(sz, {{"d", 0}});
    CHECK(s.ub.empty());
    CHECK(s.value == 16);
    CHECK(run(synth("return sizeof(int *);", {})).value == 8);
    // the 64-bit twin: long overflows at LONG_MAX + 1
    auto big = synth("long x = a; x = x + 1; return 0;", {{"long", "a"}});
    CHECK(run(big, {{"a", kI64Max}}).ub == "INT-SIGNED-OVF");
    CHECK(run(big, {{"a", kIntMax}}).ub.empty());
}

TEST_CASE("concrete: unsigned long long locals and ULL / UL literals run") {
    auto ull = synth("unsigned long long x = 5; x = x * 3; return x == 15;", {{"int", "a"}});
    auto r = run(ull, {{"a", 0}});
    CHECK(r.error.empty());
    CHECK(r.value == 1);
    auto lit = synth("unsigned long long m = 0xFFFFFFFFFFFFFFFFULL; unsigned long y = 7UL; "
                     "long long z = 10LL; return (m + 1 == 0) + (y == 7) + (z == 10) + (10ULL == 10);",
                     {{"int", "a"}});
    auto rl = run(lit, {{"a", 0}});
    CHECK(rl.error.empty());
    CHECK(rl.value == 4);
    // unsigned 64-bit wraps; its compare is unsigned
    auto wrap = synth("unsigned long long u = 0; u = u - 1; return u > 5;", {{"int", "a"}});
    CHECK(run(wrap, {{"a", 0}}).value == 1);
    // an unsigned 32-bit value widens by zero extension
    auto zext = synth("unsigned long long w = x; return w == 4294967295ULL;", {{"unsigned", "x"}});
    CHECK(run(zext, {{"x", -1}}).value == 1);
    // a decimal literal above INT_MAX is a long, not a wrapped int
    CHECK(run(synth("long long v = 4294967296; return v == 4294967296LL;", {})).value == 1);
}

TEST_CASE("concrete: casts, digit separators and narrow types") {
    // (long long)a * b is computed in 64 bits: no 32-bit overflow
    auto cast = synth("long long p = (long long)a * b; return p > 0;", {{"int", "a"}, {"int", "b"}});
    auto r = run(cast, {{"a", kIntMax}, {"b", 2}});
    CHECK(r.ub.empty());
    CHECK(r.value == 1);
    CHECK(run(synth("return a * b;", {{"int", "a"}, {"int", "b"}}), {{"a", kIntMax}, {"b", 2}}).ub ==
          "INT-SIGNED-OVF");
    CHECK(run(synth("int m = 1'000; return m;", {})).value == 1000);
    CHECK(run(synth("return 0b1010;", {})).value == 10);
    // char and unsigned char keep 8 bits
    CHECK(run(synth("unsigned char c = 300; return c;", {})).value == 44);
    CHECK(run(synth("char c = 200; return c < 0;", {})).value == 1);
    CHECK(run(synth("return (unsigned char)x;", {{"int", "x"}}), {{"x", 257}}).value == 1);
    // 1 << 31 is undefined for a signed int, fine for an unsigned one
    CHECK(run(synth("return 1 << 31;", {})).ub == "INT-SHIFT-UB");
    CHECK(run(synth("unsigned u = 1u << 31; return u > 0;", {})).value == 1);
    CHECK(run(synth("long long v = 1LL << 40; return v > 0;", {})).value == 1);
    // a float literal is not silently read as two integers
    CHECK_FALSE(run(synth("return 1.5 > 1;", {})).error.empty());
}

TEST_CASE("concrete: array initialisers") {
    auto sum = synth("int p[] = {1, 2, 3, 4}; return p[0] + p[3] + sizeof(p);", {{"int", "a"}});
    auto r = run(sum, {{"a", 0}});
    CHECK(r.error.empty());
    CHECK(r.value == 1 + 4 + 16);
    // the element count of `[]` is exact: p[4] is out of bounds
    CHECK(run(synth("int p[] = {1, 2, 3, 4}; return p[a];", {{"int", "a"}}), {{"a", 4}}).ub == "MEM-OOB-READ");
    CHECK(run(synth("int p[] = {1, 2, 3, 4}; return p[a];", {{"int", "a"}}), {{"a", 3}}).value == 4);
    // a sized initialiser keeps its values and zero-fills the rest
    CHECK(run(synth("int q[4] = {7, 8}; return q[1] * 10 + q[3];", {})).value == 80);
    CHECK(run(synth("char s[] = \"ab\"; return sizeof(s) * 100 + s[1];", {})).value == 300 + 'b');
    CHECK(run(synth("long a[2] = {4294967296, 1}; return a[0] == 4294967296;", {})).value == 1);
    // multi-declarations
    CHECK(run(synth("int a = 1, b = a + 1, c[2] = {3, 4}; return a + b + c[1];", {})).value == 7);
    // a 2-D array is not modelled: NEEDS-HARNESS, not ERROR
    auto two = run(synth("int m[2][2] = {{1, 2}, {3, 4}}; return m[0][0];", {}));
    CHECK(two.error.starts_with("UNENCODED: "));
    CHECK(prism::harness_for_parsefail(two.error, "fuzzer").has_value());
    // a variable dimension is a VLA
    CHECK(run(synth("int v[x]; return 0;"), {{"x", 3}}).error == "VLA unencoded");
}

TEST_CASE("concrete: for-init declarations and compound shifts") {
    auto f = synth("int s = 0; for (long long i = 0; i < 3; i++) s += 2; s <<= 2; return s;", {});
    auto r = run(f);
    CHECK(r.error.empty());
    CHECK(r.value == 24);
}

TEST_CASE("concrete: the return value follows the return type") {
    auto u = synth("return -1;", {}, "unsigned");
    CHECK(run(u).value == 4294967295LL);
    auto l = synth("return 4294967296LL;", {}, "long");
    CHECK(run(l).value == 4294967296LL);
    auto i = synth("return 4294967297LL;", {}, "int");
    CHECK(run(i).value == 1);
}

// ---- exact 64-bit division; stateless interpreter

TEST_CASE("concrete: 64-bit / and % are exact above 2^53") {
    std::vector<std::pair<std::string, std::string>> ll3 = {
        {"long long", "a"}, {"long long", "b"}, {"long long", "c"}};
    auto div = synth("if (a / b == c) return 1; return 0;", ll3);
    const int64_t big = 9007199254740993;  // 2^53 + 1: not a double
    CHECK(run(div, {{"a", big}, {"b", 1}, {"c", big}}).value == 1);
    CHECK(run(div, {{"a", -big}, {"b", 1}, {"c", -big}}).value == 1);
    auto rem = synth("if (a % b == c) return 1; return 0;", ll3);
    CHECK(run(rem, {{"a", kI64Max}, {"b", 10}, {"c", 7}}).value == 1);
    CHECK(run(rem, {{"a", -kI64Max}, {"b", 10}, {"c", -7}}).value == 1);
}

TEST_CASE("concrete: 32-bit division truncates toward zero") {
    CHECK(run(synth("return x / 2;"), {{"x", -7}}).value == -3);
    CHECK(run(synth("return x % 2;"), {{"x", -7}}).value == -1);
    CHECK(run(synth("return x / 0;"), {{"x", 1}}).ub == "INT-DIV-ZERO");
}

TEST_CASE("concrete: contract division (rapid's evaluator) is exact") {
    const int64_t big = 9007199254740993;
    sd::St st({}, {}, {});
    st.types["a"] = sd::CT{64, false};
    st.types["b"] = sd::CT{64, false};
    st.vars["a"] = big;
    st.vars["b"] = 1;
    CHECK(sd::eval_src(st, "a / b") == big);
    sd::St st2({}, {}, {});
    st2.vars["a"] = -7;
    st2.vars["b"] = 2;
    CHECK(sd::eval_src(st2, "a / b") == -3);
}

TEST_CASE("concrete: runs do not share state") {
    auto a = synth("int a[4]; return sizeof(a);");
    auto b = synth("int a[9]; return sizeof(a);");
    CHECK(run(a, {{"x", 0}}).value == 16);
    CHECK(run(b, {{"x", 0}}).value == 36);
    CHECK(run(a, {{"x", 0}}).value == 16);
    auto s = synth("return x / 2;");
    auto u = synth("return x / 2;", {{"unsigned", "x"}});
    CHECK(run(s, {{"x", -4}}).value == -2);
    CHECK(run(u, {{"x", -4}}).value == 0x7FFFFFFE);
    CHECK(run(s, {{"x", -4}}).value == -2);
    auto narrow = synth("return x + x;");
    auto wide = synth("return x + x;", {{"long long", "x"}}, "long long");
    CHECK(run(narrow, {{"x", 0x7FFFFFFF}}).ub == "INT-SIGNED-OVF");
    CHECK(run(wide, {{"x", 0x7FFFFFFF}}).ub.empty());
    CHECK(run(narrow, {{"x", 0x7FFFFFFF}}).ub == "INT-SIGNED-OVF");
    auto bad = synth("return (x;");
    auto first = run(bad, {{"x", 1}});
    auto second = run(bad, {{"x", 1}});
    CHECK_FALSE(first.error.empty());
    CHECK(first.error == second.error);
    CHECK(first.ub == second.ub);
    CHECK(first.value == second.value);
}

// ---- argument bytes

TEST_CASE("decode_args: little-endian int round-trips through pack_args") {
    auto f = fn("add_overflow");
    std::vector<uint8_t> data = {0xFF, 0xFF, 0xFF, 0x7F};
    auto args = sd::decode_args(f, data);
    CHECK(args["x"] == kIntMax);
    CHECK(pack_args(f, args) == data);
}

TEST_CASE("decode_args: 8-byte parameters keep their full value") {
    auto f = synth("return 0;", {{"long long", "a"}, {"size_t", "n"}, {"unsigned  long", "u"}});
    CHECK(sd::param_nbytes(f.params) == 24);
    Args in{{"a", kI64Max}, {"n", int64_t{1} << 40}, {"u", -1}};
    auto bytes = pack_args(f, in);
    REQUIRE(bytes.size() == 24);
    auto out = sd::decode_args(f, bytes);
    CHECK(out["a"] == kI64Max);
    CHECK(out["n"] == int64_t{1} << 40);
    CHECK(out["u"] == -1);  // the bit pattern of ULONG_MAX
    // `unsigned  long` (two spaces) is the 8-byte `unsigned long`
    CHECK(sd::ctype_key("unsigned  long *") == "unsigned long");
    // the long long oracle reaches a 64-bit overflow from bytes
    auto add = fn("add_ll");
    auto crash = sd::execute(add, sd::decode_args(add, pack_args(add, {{"a", kI64Max}, {"b", 1}})));
    CHECK(crash.ub == "INT-SIGNED-OVF");
}

TEST_CASE("interesting_seeds include y == 0 and x == INT_MAX") {
    auto f = fn("div_param");
    bool zero = false;
    for (auto& s : sd::interesting_seeds(f)) zero = zero || sd::decode_args(f, s)["y"] == 0;
    CHECK(zero);
    auto g = fn("add_overflow");
    bool imax = false;
    for (auto& s : sd::interesting_seeds(g)) imax = imax || sd::decode_args(g, s)["x"] == kIntMax;
    CHECK(imax);
}

TEST_CASE("bytes_from_cex keeps values above INT_MAX and packs each parameter at its width") {
    auto f = synth("return 0;", {{"unsigned", "x"}});
    auto b = sd::bytes_from_cex("x=4294967295", f);
    REQUIRE(b);
    CHECK(*b == std::vector<uint8_t>{0xFF, 0xFF, 0xFF, 0xFF});
    auto h = sd::bytes_from_cex("x=0xFFFFFFFF", f);
    REQUIRE(h);
    CHECK(*h == *b);
    CHECK(sd::bytes_from_cex("x=#xffffffff", f) == b);
    // a leading-zero decimal is ambiguous, a non-number is not a value
    CHECK_FALSE(sd::bytes_from_cex("x=010", f));
    CHECK_FALSE(sd::bytes_from_cex("x=abc", f));
    CHECK_FALSE(sd::bytes_from_cex("", f));
    auto two = synth("return 0;", {{"int", "a"}, {"long long", "b"}});
    auto t = sd::bytes_from_cex("b=-2, a=-1", two);
    REQUIRE(t);
    REQUIRE(t->size() == 12);
    auto args = sd::decode_args(two, *t);
    CHECK(args["a"] == -1);
    CHECK(args["b"] == -2);
    auto big = sd::bytes_from_cex("a=1, b=9223372036854775807", two);
    REQUIRE(big);
    CHECK(sd::decode_args(two, *big)["b"] == kI64Max);
}

// ---- the fuzzer

TEST_CASE("fuzz: planted bugs crash; a crash is not a proof") {
    const std::vector<std::pair<const char*, const char*>> cases = {
        {"add_overflow", "INT-SIGNED-OVF"},
        {"div_param", "INT-DIV-ZERO"},
        {"oob_write", "MEM-OOB-WRITE"},
        {"shift_ub", "INT-SHIFT-UB"},
    };
    for (auto& [name, cls] : cases) {
        auto r = fuzz(name, 0.5, 8);
        CHECK_MESSAGE(is(r, prism::laws::CRASH), name, " ", r.message);
        CHECK(r.cls == cls);
        CHECK_FALSE(r.counterexample.empty());
        CHECK_FALSE(prism::laws::is_proof(r.status));
        // the concrete-oracle crash row carries every extra of the CLEAN row
        for (auto* k : {"iters", "corpus", "new_cov", "noseed", "stall", "args", "oracle"})
            CHECK_MESSAGE(f_has(r, k), name, " ", k);
        CHECK(extra(r, "oracle") == "concrete");
        CHECK(extra(r, "args").find('=') != std::string::npos);
    }
}

TEST_CASE("fuzz: saturate is CLEAN, which is not a proof") {
    auto r = fuzz("saturate", 0.3, 8);
    CHECK_MESSAGE(is(r, prism::laws::CLEAN), r.message);
    CHECK_FALSE(prism::laws::is_proof(r.status));
    CHECK(r.message.find("not a proof") != std::string::npos);
    // Law 9: without --allow-exec the binary oracle did not run, and says so
    CHECK(extra(r, "binary") == std::string(prism::laws::NOTRUN));
    CHECK(extra(r, "exec") == std::string(prism::laws::NOTRUN));
}

TEST_CASE("fuzz: a 64-bit overflow is found from bytes") {
    auto r = fuzz("add_ll", 0.5, 64);
    CHECK_MESSAGE(is(r, prism::laws::CRASH), r.message);
    CHECK(r.cls == "INT-SIGNED-OVF");
}

TEST_CASE("fuzz: pointer and unmodelled functions are NEEDS-HARNESS, never ERROR or CLEAN") {
    auto p = fuzz("null_branch");
    CHECK(is(p, prism::laws::NEEDS_HARNESS));
    CHECK(p.message.find("POINTER") != std::string::npos);
    auto pf = fuse("null_branch");
    CHECK(is(pf, prism::laws::NEEDS_HARNESS));
    CHECK(pf.message.find("POINTER") != std::string::npos);
    const char* fuzz_only[] = {"asm_vol", "arr_esc_paren", "esc_paren_addr", "auto_type_bad", "static_local_bad",
                               "typedef_local_bad", "anon_enum_bad", "alignas_bad", "compound_bad",
                               "const_for_bad", "mkstemp_ok"};
    for (auto* name : fuzz_only) {
        auto r = fuzz(name);
        CHECK_MESSAGE(is(r, prism::laws::NEEDS_HARNESS), name, " ", r.status, " ", r.message);
    }
    const char* both[] = {"arr_esc_bad", "atom_qual_bad", "const_local_bad", "struct_local_bad",
                          "register_local_bad", "anon_struct_bad", "missing_nul_bad"};
    for (auto* name : both) {
        auto r = fuzz(name);
        CHECK_MESSAGE(is(r, prism::laws::NEEDS_HARNESS), name, " ", r.status, " ", r.message);
        auto rf = fuse(name);
        CHECK_MESSAGE(is(rf, prism::laws::NEEDS_HARNESS), name, " ", rf.status, " ", rf.message);
    }
    for (auto [file, name] : {std::pair{"try_catch.cpp", "try_ok"}, std::pair{"cxx_newdel.cpp", "new_no_ptr"}}) {
        auto path = td() / file;
        FunctionInfo hit;
        for (auto& f : prism::extract_functions(path, path.string()))
            if (f.name == name) hit = f;
        REQUIRE(hit.name == name);
        auto r = sd::fuzz_function(hit, path, 0.1, 1, nullptr);
        CHECK_MESSAGE(is(r, prism::laws::NEEDS_HARNESS), name, " ", r.message);
        auto recs = prism::run_fuse({hit}, {}, path.parent_path(), 0.1, 1, false);
        REQUIRE_FALSE(recs.empty());
        CHECK_MESSAGE(is(recs[0], prism::laws::NEEDS_HARNESS), name, " ", recs[0].message);
    }
}

TEST_CASE("fuzz: a function the interpreter cannot parse is not CLEAN (Law 7)") {
    // `x.y` member access: the concrete oracle cannot run it
    auto f = synth("int r = 0; r = x.y; return r;");
    auto r = sd::fuzz_function(f, td() / "no_such_file.c", 0.2, 8, nullptr);
    CHECK_FALSE(is(r, prism::laws::CLEAN));
    CHECK(is(r, prism::laws::ERROR));
    CHECK(r.message.find("concrete interpreter") != std::string::npos);
    CHECK_FALSE(extra(r, "concrete").empty());
    // a mapped reason (2-D array) is NEEDS-HARNESS with the interpreter's reason
    auto g = synth("int m[2][2] = {{1, 2}, {3, 4}}; return m[0][x & 1];");
    auto rg = sd::fuzz_function(g, td() / "no_such_file.c", 0.2, 8, nullptr);
    CHECK(is(rg, prism::laws::NEEDS_HARNESS));
    CHECK(rg.message.find("multi-dimensional") != std::string::npos);
    // 64-bit locals now run: fuzz is CLEAN (not a proof), not an error
    auto ull = synth("unsigned long long x = 5; unsigned long long m = 0xFFFFFFFFFFFFFFFFULL; "
                     "int p[] = {1, 2, 3, 4}; return (int)(x + (m & 1)) + p[a & 3];",
                     {{"int", "a"}});
    auto ru = sd::fuzz_function(ull, td() / "no_such_file.c", 0.2, 8, nullptr);
    CHECK_MESSAGE(is(ru, prism::laws::CLEAN), ru.message);
}

TEST_CASE("fuse without an LLM keeps the pointer NEEDS-HARNESS") {
    auto r = fuse("null_branch");
    CHECK(is(r, prism::laws::NEEDS_HARNESS));
    CHECK_FALSE(is(r, prism::laws::ERROR));
}

TEST_CASE("fuzz LP64: testdata_fp/long_arith.c never crashes, its testdata_tp twins do") {
    auto fp = repo() / "testdata_fp" / "long_arith.c";
    for (auto& f : prism::extract_functions(fp, fp.string())) {
        auto r = sd::fuzz_function(f, fp, 0.3, 64, nullptr);
        CHECK_MESSAGE(r.status != prism::laws::CRASH, f.name, " ", r.message);
        CHECK_MESSAGE(r.status != prism::laws::ERROR, f.name, " ", r.message);
    }
    auto tp = repo() / "testdata_tp" / "long_arith.c";
    for (auto& f : prism::extract_functions(tp, tp.string())) {
        auto r = sd::fuzz_function(f, tp, 1.0, 256, nullptr);
        CHECK_MESSAGE(is(r, prism::laws::CRASH), f.name, " ", r.message);
        CHECK(r.cls == "INT-SIGNED-OVF");
    }
}

// ---- the shared, memoized unencoded-syntax gate

TEST_CASE("unencoded_syntax_reason_cached: same answer twice, engine named") {
    int seen = 0;
    std::vector<fs::path> files;
    for (auto& e : fs::directory_iterator(td()))
        if (e.path().extension() == ".c") files.push_back(e.path());
    std::sort(files.begin(), files.end());
    for (std::size_t i = 0; i < files.size(); i += 60) {
        for (auto& f : prism::extract_functions(files[i], files[i].string())) {
            for (auto* eng : {"concolic engine", "FuSeBMC", "fuzzer", "bitvector BMC"}) {
                auto a = prism::unencoded_syntax_reason_cached(f, eng);
                auto b = prism::unencoded_syntax_reason_cached(f, eng);
                CHECK(a == b);
                // the answer depends on the engine only through its name
                auto other = prism::unencoded_syntax_reason_cached(f, "X");
                CHECK(a.has_value() == other.has_value());
            }
            ++seen;
        }
    }
    CHECK(seen > 10);
    auto cg = synth("int x; goto *p;");
    auto fz = prism::unencoded_syntax_reason_cached(cg, "FuSeBMC");
    REQUIRE(fz);
    CHECK(fz->find("FuSeBMC") != std::string::npos);
    auto fu = prism::unencoded_syntax_reason_cached(cg, "fuzzer");
    REQUIRE(fu);
    CHECK(fu->find("fuzzer") != std::string::npos);
}

TEST_CASE("unencoded_syntax_reason_cached sees a change to fn.file") {
    auto dir = fs::temp_directory_path() / "prism_concrete_memo";
    fs::create_directories(dir);
    auto path = dir / "pk.c";
    {
        std::ofstream(path) << "int f(int x) { return x; }\n";
    }
    auto f = synth("return x;", {{"int", "x"}}, "int", path.string());
    CHECK_FALSE(prism::unencoded_syntax_reason_cached(f, "fuzzer"));
    {
        std::ofstream(path) << "#pragma pack(1)\nint f(int x) { return x; }\n";
    }
    auto r = prism::unencoded_syntax_reason_cached(f, "fuzzer");
    REQUIRE(r);
    CHECK(r->find("pragma pack") != std::string::npos);
    std::error_code ec;
    fs::remove_all(dir, ec);
}

// ---- docs/FUZZ_SELF.md F2-F3: loaders never throw on bad input

TEST_CASE("F2: RunReport::load returns nullopt on a report of the wrong shape") {
    auto dir = fs::temp_directory_path() / "prism_f2_report";
    fs::create_directories(dir);
    auto p = dir / "report.json";
    for (auto* text : {R"({"stages": "x"})", R"({"functions": [1]})", R"({"stages": [1]})", "[]", R"("x")"}) {
        {
            std::ofstream(p, std::ios::binary) << text;
        }
        CHECK_MESSAGE(!prism::RunReport::load(p).has_value(), text);
    }
    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST_CASE("F3: journal lines of the wrong shape are skipped") {
    auto out = fs::temp_directory_path() / "prism_f3_journal";
    fs::create_directories(out);
    {
        std::ofstream(out / "stages.jsonl", std::ios::binary)
            << "\"\"\n[1]\n5\n{\"name\": \"lints\", \"status\": \"ok\"}\n";
    }
    auto recs = prism::journal_read_stages(out);
    REQUIRE(recs.size() == 1);
    CHECK(recs[0].name == "lints");
    CHECK(prism::journal_completed_ok(out).contains("lints"));
    std::error_code ec;
    fs::remove_all(out, ec);
}
