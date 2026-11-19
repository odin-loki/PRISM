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

namespace {

KRun stops_run(const std::string& ir, const std::string& fn, bool stops) {
    static std::vector<pp::ir::Module> keep;
    keep.push_back(pp::ir::parse_module_kernel(ir));
    const auto* f = keep.back().find(fn);
    REQUIRE(f != nullptr);
    pp::TranslateOptions o;
    o.kernel_api = "linux";
    o.kernel_stops = stops;
    auto t = pp::translate(keep.back(), *f, o);
    if (!t.fn) return {std::string(prism::laws::NEEDS_HARNESS), t.reason, {}};
    auto v = pp::check_function(*t.fn, 4, 30.0);
    return {v.status, v.message, t.fn->assumptions};
}

// BUG_ON(d == 0) then 100 / d: Linux x86 BUG() is the ud2 asm with a __bug_table entry, then unreachable
const char* kBugOn = R"IR(define i32 @bug_on_div(i32 %d) {
entry:
  %z = icmp eq i32 %d, 0
  br i1 %z, label %bug, label %ok
bug:
  call void asm sideeffect "1: ud2; .pushsection __bug_table; 2: .long 1b - .; .popsection", ""()
  unreachable
ok:
  %q = sdiv i32 100, %d
  ret i32 %q
}
define i32 @bug_on_div_o0(i32 %d) {
entry:
  %z = icmp eq i32 %d, 0
  br i1 %z, label %bug, label %ok
bug:
  call void asm sideeffect "1: .byte 0x0f, 0x0b; .pushsection __bug_table; 2: .long 1b - .; .popsection", ""()
  br label %dead
dead:
  unreachable
ok:
  %q = sdiv i32 100, %d
  ret i32 %q
}
define i32 @warn_on_div(i32 %d) {
entry:
  %z = icmp eq i32 %d, 0
  br i1 %z, label %warn, label %ok
warn:
  call void asm sideeffect "1: ud2; .pushsection __bug_table; 2: .long 1b - .; .popsection", ""()
  br label %ok
ok:
  %q = sdiv i32 100, %d
  ret i32 %q
}
define i32 @panic_div(i32 %d) {
entry:
  %z = icmp eq i32 %d, 0
  br i1 %z, label %p, label %ok
p:
  call void (ptr, ...) @panic(ptr null)
  unreachable
ok:
  %q = sdiv i32 100, %d
  ret i32 %q
}
declare void @panic(ptr, ...)
)IR";

}  // namespace

#ifdef PRISM_HAS_Z3
TEST_CASE("pir cluster answers: --pir-kernel-stops ends the path at BUG() and panic(), named; WARN() continues") {
    // without the flag: BUG() reaches unreachable (reported), panic has no model here (UNENCODED): never PROVED
    auto bug0 = stops_run(kBugOn, "bug_on_div", false);
    CHECK(bug0.status != prism::laws::PROVED);
    auto pan0 = stops_run(kBugOn, "panic_div", false);
    CHECK(pan0.status != prism::laws::PROVED);
    // with it: the path through BUG() / panic() ends; the division on the other path is guarded
    auto bug = stops_run(kBugOn, "bug_on_div", true);
    CHECK_MESSAGE(bug.status == prism::laws::PROVED, (bug.status + " " + bug.msg));
    CHECK(has(bug.assumptions, "kernel-stop: assuming the BUG() / BUG_ON() condition never triggers"));
    // the -O0 shape: the asm, a branch, then unreachable in the next block
    auto bug2 = stops_run(kBugOn, "bug_on_div_o0", true);
    CHECK_MESSAGE(bug2.status == prism::laws::PROVED, (bug2.status + " " + bug2.msg));
    auto pan = stops_run(kBugOn, "panic_div", true);
    CHECK_MESSAGE(pan.status == prism::laws::PROVED, (pan.status + " " + pan.msg));
    CHECK(has(pan.assumptions, "kernel-stop: assuming the @panic condition never triggers"));
    // WARN(): the same asm without unreachable after it continues, so d == 0 reaches the division
    auto warn = stops_run(kBugOn, "warn_on_div", true);
    CHECK_MESSAGE(warn.status == prism::laws::FAILED, (warn.status + " " + warn.msg));
}
#endif

namespace {

KRun current_run(const std::string& ir, const std::string& fn, bool cur, const char* api) {
    static std::vector<pp::ir::Module> keep;
    keep.push_back(pp::ir::parse_module_kernel(ir));
    const auto* f = keep.back().find(fn);
    REQUIRE(f != nullptr);
    pp::TranslateOptions o;
    o.kernel_api = api;
    o.kernel_current = cur;
    auto t = pp::translate(keep.back(), *f, o);
    if (!t.fn) return {std::string(prism::laws::NEEDS_HARNESS), t.reason, {}};
    auto v = pp::check_function(*t.fn, 4, 30.0);
    return {v.status, v.message, t.fn->assumptions};
}

// Linux `current->field`: this_cpu_read_stable(current_task) is an integer asm output, then inttoptr
const char* kCurrent = R"IR(@current_task = external global ptr
@other_pcpu = external global ptr
define i32 @cur_field() {
entry:
  %v = call i64 asm "movq %gs:$1, $0", "=r,i,~{dirflag},~{fpsr},~{flags}"(ptr @current_task)
  %p = inttoptr i64 %v to ptr
  %f = getelementptr inbounds i8, ptr %p, i64 44
  %x = load i32, ptr %f
  ret i32 %x
}
define i32 @other_field() {
entry:
  %v = call i64 asm "movq %gs:$1, $0", "=r,i,~{dirflag},~{fpsr},~{flags}"(ptr @other_pcpu)
  %p = inttoptr i64 %v to ptr
  %f = getelementptr inbounds i8, ptr %p, i64 44
  %x = load i32, ptr %f
  ret i32 %x
}
)IR";

}  // namespace

#ifdef PRISM_HAS_Z3
TEST_CASE("pir cluster answers: --pir-kernel-current makes Linux current a valid object; other per-CPU reads stay arbitrary") {
    auto off = current_run(kCurrent, "cur_field", false, "linux");
    CHECK(off.status != prism::laws::PROVED);
    auto on = current_run(kCurrent, "cur_field", true, "linux");
    CHECK_MESSAGE(on.status == prism::laws::PROVED, (on.status + " " + on.msg));
    CHECK(has(on.assumptions, "kernel-current: the %gs read of the running thread"));
    // a %gs read of another per-CPU variable may be NULL: never proved by this flag
    auto other = current_run(kCurrent, "other_field", true, "linux");
    CHECK(other.status != prism::laws::PROVED);
}
#endif

namespace {

KRun global_run(const std::string& ir, const std::string& fn, bool harness) {
    static std::vector<pp::ir::Module> keep;
    keep.push_back(pp::ir::parse_module_kernel(ir));
    const auto* f = keep.back().find(fn);
    REQUIRE(f != nullptr);
    pp::TranslateOptions o;
    o.kernel_api = "linux";
    o.global_harness = harness;
    auto t = pp::translate(keep.back(), *f, o);
    if (!t.fn) return {std::string(prism::laws::NEEDS_HARNESS), t.reason, {}};
    auto v = pp::check_function(*t.fn, 4, 30.0);
    return {v.status, v.message, t.fn->assumptions};
}

// emev2_smu_write-style MMIO: a static base pointer the init code sets, then base + offset
const char* kMmio = R"IR(@smu_base = internal global ptr null
@count = internal global i32 0
define void @smu_write(i32 %v) {
entry:
  %b = load ptr, ptr @smu_base
  %p = getelementptr inbounds i8, ptr %b, i64 16
  store volatile i32 %v, ptr %p
  ret void
}
define i32 @count_div() {
entry:
  %c = load i32, ptr @count
  %q = sdiv i32 100, %c
  ret i32 %q
}
)IR";

}  // namespace

#ifdef PRISM_HAS_Z3
TEST_CASE("pir cluster answers: --pir-global-harness gives mutable pointer globals a valid object; scalars stay arbitrary") {
    auto off = global_run(kMmio, "smu_write", false);
    CHECK(off.status != prism::laws::PROVED);
    auto on = global_run(kMmio, "smu_write", true);
    CHECK_MESSAGE(on.status == prism::laws::PROVED, (on.status + " " + on.msg));
    CHECK(has(on.assumptions, "global harness: @smu_base"));
    // a scalar global is not harnessed: 100 / count can divide by zero
    auto cnt = global_run(kMmio, "count_div", true);
    CHECK(cnt.status == prism::laws::FAILED);
    CHECK_FALSE(has(cnt.assumptions, "global harness"));
}
#endif

namespace {

KRun lockw_run(const std::string& ir, const std::string& fn, bool wrappers) {
    static std::vector<pp::ir::Module> keep;
    keep.push_back(pp::ir::parse_module_kernel(ir));
    const auto* f = keep.back().find(fn);
    REQUIRE(f != nullptr);
    pp::TranslateOptions o;
    o.kernel_api = "linux";
    o.lock_wrappers = wrappers;
    auto t = pp::translate(keep.back(), *f, o);
    if (!t.fn) return {std::string(prism::laws::NEEDS_HARNESS), t.reason, {}};
    auto v = pp::check_function(*t.fn, 4, 30.0);
    return {v.status, v.message, t.fn->assumptions};
}

const char* kLockW = R"IR(@big_lock = internal global [8 x i8] zeroinitializer
declare void @__prism_lock_acquire(ptr, i32)
declare void @__prism_lock_release(ptr, i32)
define void @acq_only() {
entry:
  call void @__prism_lock_acquire(ptr @big_lock, i32 0)
  ret void
}
define void @rel_only() {
entry:
  call void @__prism_lock_release(ptr @big_lock, i32 0)
  ret void
}
define void @acq_rel_leak(i32 %c) {
entry:
  call void @__prism_lock_acquire(ptr @big_lock, i32 0)
  %z = icmp eq i32 %c, 0
  br i1 %z, label %out, label %rel
rel:
  call void @__prism_lock_release(ptr @big_lock, i32 0)
  br label %out
out:
  ret void
}
)IR";

}  // namespace

#ifdef PRISM_HAS_Z3
TEST_CASE("pir cluster answers: --pir-lock-wrappers: acquire-only / release-only roots, named; mixed functions still checked") {
    CHECK(lockw_run(kLockW, "acq_only", false).status == prism::laws::FAILED);
    CHECK(lockw_run(kLockW, "rel_only", false).status == prism::laws::FAILED);
    auto a = lockw_run(kLockW, "acq_only", true);
    CHECK_MESSAGE(a.status == prism::laws::PROVED, (a.status + " " + a.msg));
    CHECK(has(a.assumptions, "lock-wrapper: @acq_only acquires locks"));
    auto r = lockw_run(kLockW, "rel_only", true);
    CHECK_MESSAGE(r.status == prism::laws::PROVED, (r.status + " " + r.msg));
    // acquires and releases: not a wrapper, the leak on c == 0 is still found
    CHECK(lockw_run(kLockW, "acq_rel_leak", true).status == prism::laws::FAILED);
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

namespace {

std::string icall_ir() {
    return std::string(R"IR(@tbl = internal global [17 x ptr] [ptr @f0, ptr @f1, ptr @f2, ptr @f3, ptr @f4, ptr @f5, ptr @f6, ptr @f7, ptr @f8, ptr @f9, ptr @f10, ptr @f11, ptr @f12, ptr @f13, ptr @f14, ptr @f15, ptr @f16]
define internal i32 @f0(i32 %x) {
entry:
  ret i32 0
}
define internal i32 @f1(i32 %x) {
entry:
  ret i32 1
}
define internal i32 @f2(i32 %x) {
entry:
  ret i32 2
}
define internal i32 @f3(i32 %x) {
entry:
  ret i32 3
}
define internal i32 @f4(i32 %x) {
entry:
  ret i32 4
}
define internal i32 @f5(i32 %x) {
entry:
  ret i32 5
}
define internal i32 @f6(i32 %x) {
entry:
  ret i32 6
}
define internal i32 @f7(i32 %x) {
entry:
  ret i32 7
}
define internal i32 @f8(i32 %x) {
entry:
  ret i32 8
}
define internal i32 @f9(i32 %x) {
entry:
  ret i32 9
}
define internal i32 @f10(i32 %x) {
entry:
  ret i32 10
}
define internal i32 @f11(i32 %x) {
entry:
  ret i32 11
}
define internal i32 @f12(i32 %x) {
entry:
  ret i32 12
}
define internal i32 @f13(i32 %x) {
entry:
  ret i32 13
}
define internal i32 @f14(i32 %x) {
entry:
  ret i32 14
}
define internal i32 @f15(i32 %x) {
entry:
  ret i32 15
}
define internal i32 @f16(i32 %x) {
entry:
  ret i32 16
}
define i32 @call_ok(i32 %k) {
entry:
  %ok = icmp ult i32 %k, 17
  br i1 %ok, label %go, label %out
go:
  %i = zext i32 %k to i64
  %slot = getelementptr inbounds [17 x ptr], ptr @tbl, i64 0, i64 %i
  %fp = load ptr, ptr %slot
  %r = call i32 %fp(i32 %k)
  ret i32 %r
out:
  ret i32 0
}
define i32 @call_div(i32 %k) {
entry:
  %ok = icmp ult i32 %k, 17
  br i1 %ok, label %go, label %out
go:
  %i = zext i32 %k to i64
  %slot = getelementptr inbounds [17 x ptr], ptr @tbl, i64 0, i64 %i
  %fp = load ptr, ptr %slot
  %r = call i32 %fp(i32 %k)
  %q = sdiv i32 100, %r
  ret i32 %q
out:
  ret i32 0
}
)IR");
}

KRun icall_run(const std::string& fn, bool havoc) {
    static std::vector<pp::ir::Module> keep;
    keep.push_back(pp::ir::parse_module_kernel(icall_ir()));
    const auto* f = keep.back().find(fn);
    REQUIRE(f != nullptr);
    pp::TranslateOptions o;
    o.kernel_api = "linux";
    o.icall_havoc = havoc;
    auto t = pp::translate(keep.back(), *f, o);
    if (!t.fn) return {std::string(prism::laws::NEEDS_HARNESS), t.reason, {}};
    auto v = pp::check_function(*t.fn, 4, 30.0);
    return {v.status, v.message, t.fn->assumptions};
}

}  // namespace

#ifdef PRISM_HAS_Z3
TEST_CASE("pir cluster answers: --pir-icall-havoc: more than 16 targets is a named havoc; the arbitrary result is checked") {
    auto off = icall_run("call_ok", false);
    CHECK(off.status == prism::laws::NEEDS_HARNESS);
    CHECK(off.msg.find("possible targets (> 16)") != std::string::npos);
    auto on = icall_run("call_ok", true);
    CHECK_MESSAGE(on.status == prism::laws::PROVED, (on.status + " " + on.msg));
    CHECK(has(on.assumptions, "icall-havoc: indirect call with 17 possible targets"));
    // the result is arbitrary: dividing by it is a violation, never proved
    CHECK(icall_run("call_div", true).status == prism::laws::FAILED);
}
#endif

namespace {

const char* kFieldInv = R"IR(%struct.dev = type { ptr, i32, i32 }
define i32 @div_by_field(ptr %d) {
entry:
  %f = getelementptr inbounds %struct.dev, ptr %d, i32 0, i32 1
  %i = load i32, ptr %f
  %q = sdiv i32 100, %i
  ret i32 %q
}
define i32 @read_buf(ptr %d) {
entry:
  %f = getelementptr inbounds %struct.dev, ptr %d, i32 0, i32 0
  %b = load ptr, ptr %f
  %e = getelementptr inbounds i8, ptr %b, i64 12
  %x = load i32, ptr %e
  ret i32 %x
}
)IR";

KRun fi_run(const std::string& fn, std::vector<pp::FieldInvariant> inv) {
    static std::vector<pp::ir::Module> keep;
    keep.push_back(pp::ir::parse_module_kernel(kFieldInv));
    const auto* f = keep.back().find(fn);
    REQUIRE(f != nullptr);
    pp::TranslateOptions o;
    o.kernel_api = "linux";
    o.auto_harness.on = true;
    o.auto_harness.depth = 0;  // nested pointer fields stay arbitrary: only the invariant can bind one
    o.field_invariants = std::move(inv);
    auto t = pp::translate(keep.back(), *f, o);
    if (!t.fn) return {std::string(prism::laws::NEEDS_HARNESS), t.reason, {}};
    auto v = pp::check_function(*t.fn, 4, 30.0);
    return {v.status, v.message, t.fn->assumptions};
}

pp::FieldInvariant fi(uint64_t off, unsigned w, const char* kind, int64_t a, int64_t b) {
    pp::FieldInvariant x;
    x.struct_type = "%struct.dev";
    x.offset = off;
    x.width = w;
    x.kind = kind;
    x.a = a;
    x.b = b;
    x.status = "UNVALIDATED";
    x.source = "test";
    return x;
}

}  // namespace

#ifdef PRISM_HAS_Z3
TEST_CASE("pir cluster answers: --pir-field-invariants: range and valid rows bind harness fields, named; wrong rows prove nothing more") {
    CHECK(fi_run("div_by_field", {}).status == prism::laws::FAILED);
    auto r = fi_run("div_by_field", {fi(8, 4, "range", 1, 10)});
    CHECK_MESSAGE(r.status == prism::laws::PROVED, (r.status + " " + r.msg));
    CHECK(has(r.assumptions, "field-invariant: d->@8 (%struct.dev at byte 8) is in [1, 10]"));
    // a range that still contains 0 does not help
    CHECK(fi_run("div_by_field", {fi(8, 4, "range", -1, 1)}).status == prism::laws::FAILED);
    // an invariant of another field does not apply
    CHECK(fi_run("div_by_field", {fi(12, 4, "range", 1, 10)}).status == prism::laws::FAILED);
    CHECK(fi_run("read_buf", {}).status != prism::laws::PROVED);
    auto v = fi_run("read_buf", {fi(0, 8, "valid", 16, 0)});
    CHECK_MESSAGE(v.status == prism::laws::PROVED, (v.status + " " + v.msg));
    CHECK(has(v.assumptions, "points to a valid object of 16 bytes"));
    // too small an object: the read at byte 12..15 of an 8-byte object is out of bounds
    CHECK(fi_run("read_buf", {fi(0, 8, "valid", 8, 0)}).status == prism::laws::FAILED);
}
#endif
