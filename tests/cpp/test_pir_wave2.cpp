// Doctests: pir UNENCODED wave 2 (docs/PIR.md): --pir-variadic, --pir-recursion, --pir-kernel-sprintf,
// --pir-addrspace, --pir-wide-int. Each option is opt-in; without it the construct stays UNENCODED. For each one a
// real violation is still FAILED (never proved), and a safe function is proved with the named assumption listed.
// Linked into prism_tests next to test_main.cpp (which provides main()).
#include <doctest/doctest.h>
#ifdef ERROR
#  undef ERROR
#endif

#include "prism/laws.hpp"
#include "prism/pir.hpp"

#include <string>
#include <vector>

namespace {

namespace pp = prism::pir;

struct W2 {
    std::string status, msg;
    std::vector<std::string> assumptions;
};

struct W2Opt {
    bool kernel = false;
    std::string api = "linux";
    bool variadic = false, recursion = false, ksprintf = false, addrspace = false, wide = false;
};

W2 run(const std::string& ir, const std::string& fn, const W2Opt& w) {
    static std::vector<pp::ir::Module> keep;
    keep.push_back(w.kernel ? pp::ir::parse_module_kernel(ir) : pp::ir::parse_module(ir));
    const auto* f = keep.back().find(fn);
    REQUIRE(f != nullptr);
    pp::TranslateOptions o;
    if (w.kernel) o.kernel_api = w.api;
    o.variadic = w.variadic;
    o.recursion = w.recursion;
    o.kernel_sprintf = w.ksprintf;
    o.addrspace = w.addrspace;
    o.wide_int = w.wide;
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

std::string all(const W2& r) {
    std::string s = r.status + " " + r.msg;
    for (auto& a : r.assumptions) s += " | " + a;
    return s;
}

// x86-64: clang -O0's lowering of va_arg(ap, int) (gp_offset / reg_save_area / overflow_arg_area)
std::string va_body(const std::string& name, const std::string& use) {
    return "define i32 @" + name + R"((i32 %n, ...) {
entry:
  %ap = alloca [1 x %struct.__va_list_tag], align 16
  call void @llvm.va_start.p0(ptr %ap)
  %gpp = getelementptr inbounds %struct.__va_list_tag, ptr %ap, i32 0, i32 0
  %gp = load i32, ptr %gpp, align 16
  %inreg = icmp ule i32 %gp, 40
  br i1 %inreg, label %reg, label %mem
reg:
  %rsap = getelementptr inbounds %struct.__va_list_tag, ptr %ap, i32 0, i32 3
  %rsa = load ptr, ptr %rsap, align 16
  %a1 = getelementptr i8, ptr %rsa, i32 %gp
  %gp2 = add i32 %gp, 8
  store i32 %gp2, ptr %gpp, align 16
  br label %got
mem:
  %ovp = getelementptr inbounds %struct.__va_list_tag, ptr %ap, i32 0, i32 2
  %ov = load ptr, ptr %ovp, align 8
  %ov2 = getelementptr i8, ptr %ov, i32 8
  store ptr %ov2, ptr %ovp, align 8
  br label %got
got:
  %a = phi ptr [ %a1, %reg ], [ %ov, %mem ]
  %x = load i32, ptr %a, align 4
  call void @llvm.va_end.p0(ptr %ap)
)" + use + "}\n";
}

std::string va_ir() {
    return "target datalayout = \"e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128\"\n"
           "%struct.__va_list_tag = type { i32, i32, ptr, ptr }\n"
           "declare void @llvm.va_start.p0(ptr)\ndeclare void @llvm.va_end.p0(ptr)\n" +
           va_body("first", "  %r = and i32 %x, 255\n  ret i32 %r\n") +
           va_body("div_first", "  %r = sdiv i32 100, %x\n  ret i32 %r\n") + R"(
define i32 @call_first(i32 %v) {
entry:
  %r = call i32 (i32, ...) @first(i32 1, i32 %v, i64 7)
  ret i32 %r
}
define i32 @call_div(i32 %v) {
entry:
  %r = call i32 (i32, ...) @div_first(i32 1, i32 %v)
  ret i32 %r
}
)";
}

// a target whose va_list is a pointer to the next argument (RISC-V)
const char* kVaPtr = R"(target datalayout = "e-m:e-p:64:64-i64:64-i128:128-n32:64-S128"
declare void @llvm.va_start.p0(ptr)
declare void @llvm.va_end.p0(ptr)
define i32 @rv_div(i32 %n, ...) {
entry:
  %ap = alloca ptr, align 8
  call void @llvm.va_start.p0(ptr %ap)
  %cur = load ptr, ptr %ap, align 8
  %nx = getelementptr inbounds i8, ptr %cur, i64 8
  store ptr %nx, ptr %ap, align 8
  %x = load i32, ptr %cur, align 4
  call void @llvm.va_end.p0(ptr %ap)
  %r = sdiv i32 100, %x
  ret i32 %r
}
)";

}  // namespace

#ifdef PRISM_HAS_Z3
TEST_CASE("pir wave2: --pir-variadic: va_start gives arbitrary variadic arguments (named); a real violation is FAILED") {
    W2Opt off, on;
    on.variadic = true;
    auto u = run(va_ir(), "first", off);
    CHECK(u.status == prism::laws::NEEDS_HARNESS);
    CHECK(u.msg.find("variadic function") != std::string::npos);
    auto p = run(va_ir(), "first", on);
    CHECK_MESSAGE(p.status == prism::laws::PROVED, all(p));
    CHECK(has(p.assumptions, "variadic: the variadic arguments of @first are arbitrary"));
    // dividing by a variadic argument: it may be 0
    auto d = run(va_ir(), "div_first", on);
    CHECK_MESSAGE(d.status == prism::laws::FAILED, all(d));
    // a call of a variadic function of the unit: inlined, the extra arguments evaluated and dropped
    auto c = run(va_ir(), "call_first", on);
    CHECK_MESSAGE(c.status == prism::laws::PROVED, all(c));
    CHECK(run(va_ir(), "call_first", off).status == prism::laws::NEEDS_HARNESS);
    auto cd = run(va_ir(), "call_div", on);
    CHECK_MESSAGE(cd.status == prism::laws::FAILED, all(cd));
    // pointer va_list (RISC-V layout)
    auto rv = run(kVaPtr, "rv_div", on);
    CHECK_MESSAGE(rv.status == prism::laws::FAILED, all(rv));
}
#endif

namespace {

const char* kRec = R"(
define i32 @fact(i32 %n) {
entry:
  %c = icmp sle i32 %n, 1
  br i1 %c, label %base, label %rec
base:
  ret i32 1
rec:
  %m = sub nsw i32 %n, 1
  %r = call i32 @fact(i32 %m)
  %p = mul nsw i32 %n, %r
  ret i32 %p
}
define i32 @depth(i32 %n) {
entry:
  %c = icmp sle i32 %n, 0
  br i1 %c, label %base, label %rec
base:
  ret i32 0
rec:
  %m = sub nsw i32 %n, 1
  %r = call i32 @depth(i32 %m)
  %q = and i32 %r, 1023
  %p = add nsw i32 %q, 1
  ret i32 %p
}
define i32 @even(i32 %n) {
entry:
  %c = icmp eq i32 %n, 0
  br i1 %c, label %yes, label %more
yes:
  ret i32 1
more:
  %m = sub i32 %n, 1
  %r = call i32 @odd(i32 %m)
  ret i32 %r
}
define i32 @odd(i32 %n) {
entry:
  %c = icmp eq i32 %n, 0
  br i1 %c, label %no, label %more
no:
  ret i32 0
more:
  %m = sub i32 %n, 1
  %r = call i32 @even(i32 %m)
  %d = sdiv i32 64, %r
  ret i32 %d
}
@g = global i32 0
define i32 @count(i32 %n) {
entry:
  store i32 5, ptr @g
  %c = icmp sle i32 %n, 0
  br i1 %c, label %base, label %rec
base:
  ret i32 0
rec:
  %m = sub nsw i32 %n, 1
  %r = call i32 @count(i32 %m)
  %v = load i32, ptr @g
  %d = sdiv i32 100, %v
  ret i32 %d
}
)";

}  // namespace

#ifdef PRISM_HAS_Z3
TEST_CASE("pir wave2: --pir-recursion: a recursive call is the function's own verdict (named), memory havocked") {
    W2Opt off, on;
    on.recursion = true;
    auto u = run(kRec, "depth", off);
    CHECK(u.status == prism::laws::NEEDS_HARNESS);
    CHECK(u.msg.find("UNENCODED: recursive call @depth") != std::string::npos);
    auto p = run(kRec, "depth", on);
    CHECK_MESSAGE(p.status == prism::laws::PROVED, all(p));
    CHECK(has(p.assumptions, "recursive-call: @depth at line"));
    CHECK(has(p.assumptions, "induction hypothesis"));
    // n! overflows: the recursive result is arbitrary, n * r overflows
    auto f = run(kRec, "fact", on);
    CHECK_MESSAGE(f.status == prism::laws::FAILED, all(f));
    // mutual recursion back to the analysed function: the inner @even result may be 0
    auto e = run(kRec, "even", on);
    CHECK_MESSAGE(e.status == prism::laws::FAILED, all(e));
    // the recursive call may change memory: @g is no longer 5 after it (a divisor of 0 is reachable)
    auto c = run(kRec, "count", on);
    CHECK_MESSAGE(c.status == prism::laws::FAILED, all(c));
}
#endif

namespace {

const char* kSpr = R"(
@.d = private unnamed_addr constant [4 x i8] c"%d\0A\00"
@.ps = private unnamed_addr constant [4 x i8] c"%pS\00"
@.pv = private unnamed_addr constant [4 x i8] c"%pV\00"
@.sn = private unnamed_addr constant [3 x i8] c"%s\00"
@sym = global i32 0
declare i32 @sprintf(ptr, ptr, ...)
define i32 @show_ok(i32 %v) {
entry:
  %buf = alloca [16 x i8], align 16
  %r = call i32 (ptr, ptr, ...) @sprintf(ptr %buf, ptr @.d, i32 %v)
  ret i32 %r
}
define i32 @show_small(i32 %v) {
entry:
  %buf = alloca [8 x i8], align 1
  %r = call i32 (ptr, ptr, ...) @sprintf(ptr %buf, ptr @.d, i32 %v)
  ret i32 %r
}
define i32 @sym_ok() {
entry:
  %buf = alloca [1100 x i8], align 16
  %r = call i32 (ptr, ptr, ...) @sprintf(ptr %buf, ptr @.ps, ptr @sym)
  ret i32 %r
}
define i32 @sym_small() {
entry:
  %buf = alloca [128 x i8], align 16
  %r = call i32 (ptr, ptr, ...) @sprintf(ptr %buf, ptr @.ps, ptr @sym)
  ret i32 %r
}
define i32 @pv() {
entry:
  %buf = alloca [4096 x i8], align 16
  %r = call i32 (ptr, ptr, ...) @sprintf(ptr %buf, ptr @.pv, ptr @sym)
  ret i32 %r
}
define i32 @null_str() {
entry:
  %buf = alloca [9 x i8], align 16
  %r = call i32 (ptr, ptr, ...) @sprintf(ptr %buf, ptr @.sn, ptr null)
  ret i32 %r
}
define i32 @null_str_small() {
entry:
  %buf = alloca [8 x i8], align 16
  %r = call i32 (ptr, ptr, ...) @sprintf(ptr %buf, ptr @.sn, ptr null)
  ret i32 %r
}
)";

}  // namespace

#ifdef PRISM_HAS_Z3
TEST_CASE("pir wave2: --pir-kernel-sprintf: sound output bounds incl. %p extensions; overflows are FAILED") {
    W2Opt on;
    on.kernel = true;
    on.ksprintf = true;
    W2Opt off = on;
    off.ksprintf = false;
    auto u = run(kSpr, "show_ok", off);
    CHECK(u.status != prism::laws::PROVED);
    auto p = run(kSpr, "show_ok", on);
    CHECK_MESSAGE(p.status == prism::laws::PROVED, all(p));
    CHECK(has(p.assumptions, "linux-sprintf: kernel sprintf"));
    // "%d\n" may need 14 characters and a NUL: an 8-byte buffer overflows
    auto s = run(kSpr, "show_small", on);
    CHECK_MESSAGE(s.status == prism::laws::FAILED, all(s));
    CHECK_MESSAGE(run(kSpr, "sym_ok", on).status == prism::laws::PROVED, all(run(kSpr, "sym_ok", on)));
    CHECK(run(kSpr, "sym_small", on).status == prism::laws::FAILED);
    auto v = run(kSpr, "pv", on);
    CHECK(v.status == prism::laws::NEEDS_HARNESS);
    CHECK(v.msg.find("'%pV'") != std::string::npos);
    // %s of NULL prints "(null)" in the kernel: up to 8 characters ("(efault)") and the NUL
    CHECK_MESSAGE(run(kSpr, "null_str", on).status == prism::laws::PROVED, all(run(kSpr, "null_str", on)));
    CHECK(run(kSpr, "null_str_small", on).status == prism::laws::FAILED);
}
#endif

namespace {

const char* kSeg = R"(
%struct.pcpu = type { ptr, i32, i32 }
@cpuid.__pc = internal global ptr addrspace(256) null, align 8
@cpudiv.__pc = internal global ptr addrspace(256) null, align 8
@written.__pc = internal global ptr addrspace(256) null, align 8
define i32 @cpuid() {
entry:
  %p = load ptr addrspace(256), ptr @cpuid.__pc, align 8
  %f = getelementptr inbounds %struct.pcpu, ptr addrspace(256) %p, i32 0, i32 1
  %v = load i32, ptr addrspace(256) %f, align 4
  %r = and i32 %v, 255
  ret i32 %r
}
define i32 @cpudiv(i32 %x) {
entry:
  %p = load ptr addrspace(256), ptr @cpudiv.__pc, align 8
  %f = getelementptr inbounds %struct.pcpu, ptr addrspace(256) %p, i32 0, i32 2
  %v = load i32, ptr addrspace(256) %f, align 4
  %r = sdiv i32 %x, %v
  ret i32 %r
}
define void @bump() {
entry:
  %v = load i64, ptr addrspace(256) inttoptr (i64 16 to ptr addrspace(256)), align 8
  %w = add nsw i64 %v, 1
  store i64 %w, ptr addrspace(256) inttoptr (i64 16 to ptr addrspace(256)), align 8
  ret void
}
define i32 @far() {
entry:
  %v = load i32, ptr addrspace(256) inttoptr (i64 2000000 to ptr addrspace(256)), align 4
  ret i32 %v
}
define void @set_written(ptr addrspace(256) %q) {
entry:
  store ptr addrspace(256) %q, ptr @written.__pc, align 8
  ret void
}
define i32 @read_written() {
entry:
  %p = load ptr addrspace(256), ptr @written.__pc, align 8
  %v = load i32, ptr addrspace(256) %p, align 4
  ret i32 %v
}
)";

}  // namespace

#ifdef PRISM_HAS_Z3
TEST_CASE("pir wave2: --pir-addrspace: segment pointers are the per-CPU area (named); violations FAILED") {
    W2Opt on;
    on.kernel = true;
    on.api = "freebsd";
    on.addrspace = true;
    W2Opt off = on;
    off.addrspace = false;
    auto u = run(kSeg, "cpuid", off);
    CHECK(u.status == prism::laws::NEEDS_HARNESS);
    CHECK(u.msg.find("ptr addrspace") != std::string::npos);
    auto p = run(kSeg, "cpuid", on);
    CHECK_MESSAGE(p.status == prism::laws::PROVED, all(p));
    CHECK(has(p.assumptions, "addrspace: an access through a segment pointer"));
    CHECK_MESSAGE(run(kSeg, "cpudiv", on).status == prism::laws::FAILED, all(run(kSeg, "cpudiv", on)));
    // a per-CPU counter: add nsw of an arbitrary value overflows
    CHECK(run(kSeg, "bump", on).status == prism::laws::FAILED);
    // beyond the per-CPU area: out of bounds
    CHECK(run(kSeg, "far", on).status == prism::laws::FAILED);
    // a segment-pointer global that the unit writes is not taken as null: its arbitrary value is not proved
    CHECK(run(kSeg, "read_written", on).status != prism::laws::PROVED);
}
#endif

namespace {

const char* kWide = R"(
define i64 @mulhi(i64 %a, i64 %b) {
entry:
  %x = zext i64 %a to i128
  %y = zext i64 %b to i128
  %p = mul nuw i128 %x, %y
  %h = lshr i128 %p, 64
  %r = trunc i128 %h to i64
  ret i64 %r
}
define i64 @sq(i64 %a) {
entry:
  %x = sext i64 %a to i128
  %p = mul nsw i128 %x, %x
  %q = mul nsw i128 %p, %p
  %r = trunc i128 %q to i64
  ret i64 %r
}
define i64 @divmin(i1 %c) {
entry:
  %y = select i1 %c, i128 -170141183460469231731687303715884105728, i128 5
  %q = sdiv i128 %y, -1
  %r = trunc i128 %q to i64
  ret i64 %r
}
define i64 @divmin_ok(i1 %c) {
entry:
  %y = select i1 %c, i128 -170141183460469231731687303715884105728, i128 5
  %q = sdiv i128 %y, -2
  %r = trunc i128 %q to i64
  ret i64 %r
}
define i8 @bitfield72() {
entry:
  %s = alloca i72, align 8
  store i72 1180591620717411303424, ptr %s, align 8
  %v = load i72, ptr %s, align 8
  %h = lshr i72 %v, 64
  %t = trunc i72 %h to i8
  %q = udiv i8 100, %t
  ret i8 %q
}
define i8 @bitfield72_zero() {
entry:
  %s = alloca i72, align 8
  store i72 18446744073709551615, ptr %s, align 8
  %v = load i72, ptr %s, align 8
  %h = lshr i72 %v, 64
  %t = trunc i72 %h to i8
  %q = udiv i8 100, %t
  ret i8 %q
}
define i8 @bf160_neg() {
entry:
  %s = alloca i160, align 8
  store i160 -18446744073709551617, ptr %s, align 8
  %v = load i160, ptr %s, align 8
  %h = lshr i160 %v, 152
  %t = trunc i160 %h to i8
  %q = udiv i8 100, %t
  ret i8 %q
}
define i8 @bf160_pos() {
entry:
  %s = alloca i160, align 8
  store i160 18446744073709551617, ptr %s, align 8
  %v = load i160, ptr %s, align 8
  %h = lshr i160 %v, 152
  %t = trunc i160 %h to i8
  %q = udiv i8 100, %t
  ret i8 %q
}
define i128 @ret128(i64 %a) {
entry:
  %x = zext i64 %a to i128
  ret i128 %x
}
)";

}  // namespace

#ifdef PRISM_HAS_Z3
TEST_CASE("pir wave2: --pir-wide-int: 128-bit bit-vectors; INT128_MIN / -1 and overflow are FAILED") {
    W2Opt off, on;
    on.wide = true;
    auto u = run(kWide, "mulhi", off);
    CHECK(u.status == prism::laws::NEEDS_HARNESS);
    CHECK(u.msg.find("wider than 64 bits") != std::string::npos);
    CHECK_MESSAGE(run(kWide, "mulhi", on).status == prism::laws::PROVED, all(run(kWide, "mulhi", on)));
    CHECK(run(kWide, "sq", on).status == prism::laws::FAILED);
    auto d = run(kWide, "divmin", on);
    CHECK_MESSAGE(d.status == prism::laws::FAILED, all(d));
    CHECK(d.msg.find("INT-SIGNED-OVF") != std::string::npos);
    CHECK_MESSAGE(run(kWide, "divmin_ok", on).status == prism::laws::PROVED, all(run(kWide, "divmin_ok", on)));
    // a literal above 64 bits: 2^70 >> 64 = 64 (nonzero divisor); 2^64 - 1 >> 64 = 0
    CHECK_MESSAGE(run(kWide, "bitfield72", on).status == prism::laws::PROVED, all(run(kWide, "bitfield72", on)));
    CHECK(run(kWide, "bitfield72_zero", on).status == prism::laws::FAILED);
    // wider than 128 bits (a bitfield container): a negative literal is sign-extended past bit 127, a positive one not
    CHECK_MESSAGE(run(kWide, "bf160_neg", on).status == prism::laws::PROVED, all(run(kWide, "bf160_neg", on)));
    CHECK(run(kWide, "bf160_pos", on).status == prism::laws::FAILED);
    // parameters and results stay at most 64 bits
    auto r = run(kWide, "ret128", on);
    CHECK(r.status == prism::laws::NEEDS_HARNESS);
    CHECK(r.msg.find("return type i128") != std::string::npos);
}
#endif
