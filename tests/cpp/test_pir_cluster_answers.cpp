// Doctests: library-model frames do not count toward the pir call-inlining depth (TranslateOptions::model_inline_depth),
// and a call past either depth bound is UNENCODED (NEEDS-HARNESS), never PROVED.
// Linked into prism_tests next to test_main.cpp (which provides main()).
#include <doctest/doctest.h>
#ifdef ERROR
#  undef ERROR
#endif

#include "prism/laws.hpp"
#include "prism/pir.hpp"

#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace {

namespace pp = prism::pir;

// @top(i32 %x) -> @c1 -> ... -> @cN; @cN divides 100 by its argument when `bad`, else returns it.
// Functions whose index is in `models` are marked as PRISM library models (is_model), as link_models() does.
std::string chain_ir(int n, bool bad) {
    std::ostringstream o;
    o << "define i32 @top(i32 %x) {\nentry:\n  %r = call i32 @c1(i32 %x)\n  ret i32 %r\n}\n";
    for (int i = 1; i <= n; ++i) {
        o << "define i32 @c" << i << "(i32 %x) {\nentry:\n";
        if (i < n) {
            o << "  %r = call i32 @c" << i + 1 << "(i32 %x)\n  ret i32 %r\n}\n";
        } else if (bad) {
            o << "  %r = sdiv i32 100, %x\n  ret i32 %r\n}\n";
        } else {
            o << "  ret i32 %x\n}\n";
        }
    }
    return o.str();
}

std::string run(int n, bool bad, const std::set<int>& models, int depth, int model_depth, std::string* msg = nullptr,
                int small = 0) {
    static std::vector<pp::ir::Module> keep;  // translations point into their module
    keep.push_back(pp::ir::parse_module(chain_ir(n, bad)));
    auto& m = keep.back();
    for (auto& f : m.functions)
        if (f.name.size() > 1 && f.name[0] == 'c' && models.count(std::stoi(f.name.substr(1)))) f.is_model = true;
    const auto* f = m.find("top");
    REQUIRE(f != nullptr);
    pp::TranslateOptions o;
    o.inline_depth = depth;
    o.model_inline_depth = model_depth;
    o.small_inline_insts = small;  // the depth rules alone (the small-tree exemption has its own test)
    auto t = pp::translate(m, *f, o);
    if (!t.fn) {
        if (msg) *msg = t.reason;
        return std::string(prism::laws::NEEDS_HARNESS);  // UNENCODED reasons are reported as NEEDS-HARNESS by the pir stage
    }
    auto v = pp::check_function(*t.fn, 4, 30.0);
    if (msg) *msg = v.message;
    return v.status;
}

std::set<int> all(int n) {
    std::set<int> s;
    for (int i = 1; i <= n; ++i) s.insert(i);
    return s;
}

}  // namespace

#ifdef PRISM_HAS_Z3
TEST_CASE("pir model depth: user call chains past inline_depth stay UNENCODED, never PROVED") {
    // 6 user frames below @top with depth 4: the safe chain is not proved, the buggy one is not refuted either
    for (bool bad : {false, true}) {
        std::string msg;
        const auto st = run(6, bad, {}, 4, 16, &msg);
        CHECK(st == prism::laws::NEEDS_HARNESS);
        CHECK(st != prism::laws::PROVED);
        CHECK(msg.find("UNENCODED: call depth > 4") != std::string::npos);
    }
    // within the bound the chain is decided exactly
    CHECK(run(4, false, {}, 4, 16) == prism::laws::PROVED);
    CHECK(run(4, true, {}, 4, 16) == prism::laws::FAILED);
}

TEST_CASE("pir model depth: library-model frames are exempt from inline_depth and inlined exactly") {
    // @c1..@c6 are models: depth 4 no longer blocks them; the model's own check is decided
    CHECK(run(6, false, all(6), 4, 16) == prism::laws::PROVED);
    CHECK(run(6, true, all(6), 4, 16) == prism::laws::FAILED);
    // model_inline_depth 0: the old rule, models count like user code
    std::string msg;
    CHECK(run(6, false, all(6), 4, 0, &msg) == prism::laws::NEEDS_HARNESS);
    CHECK(msg.find("UNENCODED: call depth > 4") != std::string::npos);
}

TEST_CASE("pir model depth: a model chain past inline_depth + model_inline_depth is UNENCODED, never PROVED") {
    // 12 model frames, cap 4 + 4 = 8
    for (bool bad : {false, true}) {
        std::string msg;
        const auto st = run(12, bad, all(12), 4, 4, &msg);
        CHECK(st == prism::laws::NEEDS_HARNESS);
        CHECK(msg.find("UNENCODED: call depth > 8") != std::string::npos);
        CHECK(msg.find("library model frames") != std::string::npos);
    }
}

TEST_CASE("pir model depth: user frames below model frames still count toward inline_depth") {
    // c1..c3 models, c4..c9 user code: 6 user frames past depth 4 below the models -> UNENCODED
    std::string msg;
    CHECK(run(9, false, {1, 2, 3}, 4, 16, &msg) == prism::laws::NEEDS_HARNESS);
    CHECK(msg.find("UNENCODED: call depth > 4") != std::string::npos);
    // c1..c3 models, c4..c7 user: 4 user frames below @top's frame... top + 4 user = within depth 4
    CHECK(run(7, false, {1, 2, 3}, 4, 16) == prism::laws::PROVED);
}
#endif

namespace {

struct KRun {
    std::string status, msg;
    std::vector<std::string> assumptions;
};

KRun kernel_run(const std::string& ir, const std::string& fn) {
    static std::vector<pp::ir::Module> keep;
    keep.push_back(pp::ir::parse_module_kernel(ir));
    const auto* f = keep.back().find(fn);
    REQUIRE(f != nullptr);
    pp::TranslateOptions o;
    o.kernel_api = "linux";
    o.builtin_contracts = true;
    auto t = pp::translate(keep.back(), *f, o);
    if (!t.fn) return {std::string(prism::laws::NEEDS_HARNESS), t.reason, {}};
    auto v = pp::check_function(*t.fn, 4, 30.0);
    return {v.status, v.message, t.fn->assumptions};
}

bool has(const std::vector<std::string>& v, const std::string& s) {
    for (auto& x : v)
        if (x.find(s) != std::string::npos) return true;
    return false;
}

const char* kCpuid = R"IR(define i32 @cpuid_sum(i32 %leaf) {
entry:
  %r = call { i32, i32, i32, i32 } asm sideeffect "cpuid", "={ax},={bx},={cx},={dx},0,~{dirflag},~{fpsr},~{flags}"(i32 %leaf)
  %a = extractvalue { i32, i32, i32, i32 } %r, 0
  %d = extractvalue { i32, i32, i32, i32 } %r, 3
  %x = xor i32 %a, %d
  ret i32 %x
}
define i32 @cpuid_div(i32 %leaf) {
entry:
  %r = call { i32, i32, i32, i32 } asm sideeffect "cpuid", "={ax},={bx},={cx},={dx},0,~{dirflag},~{fpsr},~{flags}"(i32 %leaf)
  %c = extractvalue { i32, i32, i32, i32 } %r, 2
  %q = udiv i32 100, %c
  ret i32 %q
}
)IR";

}  // namespace

#ifdef PRISM_HAS_Z3
TEST_CASE("pir cluster answers: inline asm with four direct outputs, each field arbitrary (--kernel-api)") {
    auto ok = kernel_run(kCpuid, "cpuid_sum");
    CHECK_MESSAGE(ok.status == prism::laws::PROVED, (ok.status + " " + ok.msg));
    CHECK(has(ok.assumptions, "4 output(s) havocked"));
    // a field is arbitrary: dividing by it is a violation, never proved
    auto bad = kernel_run(kCpuid, "cpuid_div");
    CHECK_MESSAGE(bad.status == prism::laws::FAILED, (bad.status + " " + bad.msg));
}

TEST_CASE("pir cluster answers: llvm.write_register of the stack pointer only (--pir-builtin-contracts)") {
    const std::string body = R"IR(define void @sp_wb(i64 %v) {
entry:
  call void @llvm.write_register.i64(metadata !0, i64 %v)
  ret void
}
declare void @llvm.write_register.i64(metadata, i64)
)IR";
    auto sp = kernel_run(body + "!0 = !{!\"rsp\"}\n", "sp_wb");
    CHECK_MESSAGE(sp.status == prism::laws::PROVED, (sp.status + " " + sp.msg));
    CHECK(has(sp.assumptions, "builtin-contract: @llvm.write_register of the stack pointer rsp"));
    auto rax = kernel_run(body + "!0 = !{!\"rax\"}\n", "sp_wb");
    CHECK(rax.status == prism::laws::NEEDS_HARNESS);
    CHECK(rax.msg.find("UNENCODED: call @llvm.write_register.i64 (register rax)") != std::string::npos);
}
#endif

#ifdef PRISM_HAS_Z3
TEST_CASE("pir model depth: a small call tree is inlined past inline_depth exactly; a large one stays UNENCODED") {
    // 6 one-call wrappers: every tree below the depth bound is a few instructions
    CHECK(run(6, false, {}, 4, 16, nullptr, 64) == prism::laws::PROVED);
    CHECK(run(6, true, {}, 4, 16, nullptr, 64) == prism::laws::FAILED);
    // 40 wrappers: the tree at the bound is over 64 instructions, so the call is UNENCODED, never PROVED
    for (bool bad : {false, true}) {
        std::string msg;
        CHECK(run(40, bad, {}, 4, 16, &msg, 64) == prism::laws::NEEDS_HARNESS);
        CHECK(msg.find("UNENCODED: call depth > 4") != std::string::npos);
    }
    // the total cap holds for small trees too: 30 small frames past 4 + 16
    std::string msg;
    CHECK(run(30, false, {}, 4, 16, &msg, 4096) == prism::laws::NEEDS_HARNESS);
    CHECK(msg.find("UNENCODED: call depth > 4") != std::string::npos);
}
#endif

#ifdef PRISM_HAS_Z3
TEST_CASE("pir cluster answers: --pir-addr-values: a constant address plus a huge constant offset points to no object") {
    const char* ir = R"IR(define i32 @abs_read() {
entry:
  %x = load i32, ptr getelementptr (i8, ptr inttoptr (i64 4096 to ptr), i64 4611686018427387904)
  ret i32 %x
}
)IR";
    static std::vector<pp::ir::Module> keep;
    keep.push_back(pp::ir::parse_module_kernel(ir));
    const auto* f = keep.back().find("abs_read");
    REQUIRE(f != nullptr);
    pp::TranslateOptions o;
    o.kernel_api = "linux";
    auto t0 = pp::translate(keep.back(), *f, o);
    CHECK(!t0.fn);  // without the address model: UNENCODED
    o.addr_values = true;
    auto t = pp::translate(keep.back(), *f, o);
    REQUIRE(t.fn);
    CHECK(has(t.fn->assumptions, "addr-values: a constant address plus a constant offset"));
    // the read through an address that points to no object is a violation (NEEDS-HARNESS in the stage), never proved
    auto v = pp::check_function(*t.fn, 4, 30.0);
    CHECK(v.status == prism::laws::FAILED);
}
#endif
