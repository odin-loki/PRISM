// Doctests: the pir stage on ILP32 (SV-COMP --data-model ILP32,
// docs/SVCOMP.md "Data models"). Units are lowered for i386: int/long/size_t
// and pointers are 32 bits wide. Every width-dependent case has a twin whose
// verdict flips with the data model, so a pir that silently kept LP64 fails.
// Linked into prism_tests next to test_main.cpp (which provides main()).
#include <doctest/doctest.h>
#ifdef ERROR
#  undef ERROR
#endif

#include "prism/config.hpp"
#include "prism/laws.hpp"
#include "prism/pir.hpp"
#include "prism/pir/frontend.hpp"

#include "../../src/prism/pir/translate_mem.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;
namespace pp = prism::pir;

const char* kI386 = "e-m:e-p:32:32-p270:32:32-p271:32:32-p272:64:64-i128:128-f64:32:64-f80:32-n8:16:32-S128";

bool proof(const std::string& st) { return st.starts_with("PROVED"); }

struct Dir {
    fs::path p;
    Dir() {
        p = fs::temp_directory_path() /
            ("prism-ilp32-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(p);
    }
    ~Dir() {
        std::error_code ec;
        fs::remove_all(p, ec);
    }
};

// fn -> finding, for one C file analysed under data_model
std::map<std::string, prism::Finding> pir_on(const std::string& src, const std::string& data_model) {
    Dir d;
    auto c = d.p / "t.c";
    std::ofstream(c) << src;
    auto cfg = prism::default_config();
    cfg.root = d.p;
    cfg.jobs = 1;
    cfg.data_model = data_model;
    cfg.solver_cache = d.p / "cache";
    std::map<std::string, prism::Finding> by;
    for (auto& f : pp::run_pir({c}, cfg))
        if (f.function) by[*f.function] = f;
    return by;
}

pp::Translation tr32(const std::string& body, const std::string& fn) {
    auto m = pp::ir::parse_module(std::string("target datalayout = \"") + kI386 +
                                  "\"\ntarget triple = \"i386-unknown-linux-gnu\"\n" + body);
    const auto* f = m.find(fn);
    REQUIRE(f != nullptr);
    return pp::translate(m, *f, {});
}

}  // namespace

TEST_CASE("pir ilp32: i386 data layout (i64 and double 4-aligned, 4-byte pointers)") {
    auto m = pp::ir::parse_module(std::string("target datalayout = \"") + kI386 +
                                  "\"\n%struct.S = type { i8, i64, double, ptr }\n");
    pp::pirmem::Layout lay(m);
    auto S = pp::ir::parse_type("%struct.S");
    CHECK(lay.ptr_bytes() == 4);
    CHECK(lay.field_offset(S, 1) == 4);
    CHECK(lay.field_offset(S, 2) == 12);
    CHECK(lay.field_offset(S, 3) == 20);
    CHECK(lay.alloc_size(S) == 24);
    CHECK(lay.align(S) == 4);
    CHECK(lay.holds_ptr(S));
    CHECK_FALSE(lay.holds_ptr(pp::ir::parse_type("[4 x i32]")));
    // the same struct on x86-64 (its datalayout states i64:64)
    auto m64 = pp::ir::parse_module(
        "target datalayout = \"e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128\"\n"
        "%struct.S = type { i8, i64, double, ptr }\n");
    pp::pirmem::Layout l64(m64);
    CHECK(l64.ptr_bytes() == 8);
    CHECK(l64.field_offset(S, 1) == 8);
    CHECK(l64.field_offset(S, 2) == 16);
    CHECK(l64.field_offset(S, 3) == 24);
    CHECK(l64.alloc_size(S) == 32);
}

TEST_CASE("pir ilp32: pointer cells and wrapping getelementptr are refused, never proved") {
    // a pointer stored in memory: a 4-byte cell PIR's 64-bit pointer values do not fit
    auto st = tr32(R"IR(
define i32 @f(ptr %p) {
entry:
  %a = alloca [1 x ptr], align 4
  %e = getelementptr inbounds [1 x ptr], ptr %a, i32 0, i32 0
  store ptr %p, ptr %e, align 4
  ret i32 0
}
)IR", "f");
    CHECK_FALSE(st.fn.has_value());
    CHECK(st.reason.find("ILP32 pointer cells not modelled") != std::string::npos);
    // a load of a pointer, too
    auto ld = tr32(R"IR(
define i32 @g(ptr %p) {
entry:
  %q = load ptr, ptr %p, align 4
  %c = icmp eq ptr %q, null
  %r = zext i1 %c to i32
  ret i32 %r
}
)IR", "g");
    CHECK_FALSE(ld.fn.has_value());
    CHECK(ld.reason.find("ILP32 pointer cells not modelled") != std::string::npos);
    // no inbounds: the 32-bit offset wraps
    auto wrap = tr32(R"IR(
define i32 @h(ptr %p, i32 %i) {
entry:
  %q = getelementptr i32, ptr %p, i32 %i
  %v = load i32, ptr %q, align 4
  ret i32 %v
}
)IR", "h");
    CHECK_FALSE(wrap.fn.has_value());
    CHECK(wrap.reason.find("without inbounds") != std::string::npos);
    // a 64-bit variable index is truncated to 32 bits by LLVM
    auto wide = tr32(R"IR(
define i32 @k(ptr %p, i64 %i) {
entry:
  %q = getelementptr inbounds i32, ptr %p, i64 %i
  %v = load i32, ptr %q, align 4
  ret i32 %v
}
)IR", "k");
    CHECK_FALSE(wide.fn.has_value());
    CHECK(wide.reason.find("64-bit index") != std::string::npos);
    // i32 data through an inbounds i32 index stays encoded
    auto ok = tr32(R"IR(
define i32 @ok(ptr %p, i32 %i) {
entry:
  %q = getelementptr inbounds i32, ptr %p, i32 %i
  %v = load i32, ptr %q, align 4
  ret i32 %v
}
)IR", "ok");
    CHECK(ok.fn.has_value());
}

TEST_CASE("pir ilp32: 32-bit long / size_t / pointer twins (skips without clang/opt/z3)") {
    std::string why;
    if (!pp::frontend_usable(prism::default_config(), &why)) {
        MESSAGE(why);
        return;
    }
    const std::string src = R"C(
#include <stddef.h>
/* long is 32 bits on ILP32: int -> long -> *2 overflows there, not on LP64 */
long long_twice(int a) { long b = a; return b * 2L; }
/* the true twin: long long is 64 bits on both */
long long llong_twice(int a) { long long b = a; return b * 2LL; }
/* LONG_MAX + 1: overflows on ILP32 only */
long long_max_inc(long a) {
    if (a != 2147483647L) return 0;
    return a + 1L;
}
/* size_t is 32 bits: (int)(SIZE_MAX >> 31) is 1 on ILP32 (1 + INT_MAX
   overflows) and -1 on LP64 (no overflow) */
int size_t_bits(size_t n) {
    if (n != (size_t)-1) return 0;
    int k = (int)(n >> 31);
    return k + 2147483647;
}
/* sizeof(void*) is 4: 2^28 * 4 fits in int, 2^28 * 8 does not */
int ptr_scale(int x) {
    if (x != 268435456) return 0;
    return x * (int)sizeof(void *);
}
/* bounded long arithmetic: fine on both */
int long_ok(long a) {
    if (a > 1000L || a < -1000L) return 0;
    return (int)(a * 1000L);
}
/* a pointer in memory: refused on ILP32 (never proved) */
int ptr_cell(int *p) {
    int *q[2];
    q[0] = p;
    q[1] = p;
    return q[0] == q[1];
}
)C";
    auto i32 = pir_on(src, "ILP32");
    auto i64 = pir_on(src, "LP64");
    for (const char* fn : {"long_twice", "llong_twice", "long_max_inc", "size_t_bits", "ptr_scale", "long_ok", "ptr_cell"}) {
        INFO(fn);
        REQUIRE(i32.contains(fn));
        REQUIRE(i64.contains(fn));
    }
    // ILP32 findings say how the unit was lowered
    CHECK(i32["long_ok"].extra.at("target") == pp::ilp32_target_triple());
    CHECK(i32["long_ok"].extra.at("data_model") == "ILP32");
    CHECK_FALSE(i64["long_ok"].extra.contains("target"));

    CHECK(i32["long_twice"].status == prism::laws::FAILED);
    CHECK(proof(i64["long_twice"].status));
    CHECK(proof(i32["llong_twice"].status));
    CHECK(proof(i64["llong_twice"].status));
    CHECK(i32["long_max_inc"].status == prism::laws::FAILED);
    CHECK(proof(i64["long_max_inc"].status));
    CHECK(i32["size_t_bits"].status == prism::laws::FAILED);
    CHECK(proof(i64["size_t_bits"].status));
    CHECK(proof(i32["ptr_scale"].status));
    CHECK(i64["ptr_scale"].status == prism::laws::FAILED);
    CHECK(proof(i32["long_ok"].status));
    CHECK(proof(i64["long_ok"].status));
    CHECK_FALSE(proof(i32["ptr_cell"].status));
    CHECK(i32["ptr_cell"].status != prism::laws::FAILED);
    CHECK(i32["ptr_cell"].message.find("ILP32 pointer cells not modelled") != std::string::npos);
}
