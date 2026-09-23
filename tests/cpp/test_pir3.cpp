// Doctests: PIR floating point, exceptions, setjmp/longjmp, indirect calls,
// coroutines and inline assembly (roadmap 2.3 / 2.6; docs/PIR.md).
// Linked into prism_tests next to test_main.cpp (which provides main()).
#include <doctest/doctest.h>
#ifdef ERROR
#  undef ERROR
#endif

#include "prism/config.hpp"
#include "prism/laws.hpp"
#include "prism/pir.hpp"

#include "../../src/prism/pir/fp.hpp"
#include "../../src/prism/pir/lower_ctl.hpp"
#include "../../src/prism/pir/stage_mem.hpp"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

namespace pp = prism::pir;
namespace fp = prism::pir::fp;

uint64_t dbits(double d) {
    uint64_t u;
    std::memcpy(&u, &d, 8);
    return u;
}

uint64_t fbits(float f) {
    uint32_t u;
    std::memcpy(&u, &f, 4);
    return u;
}

pp::Translation tr(const std::string& ir, const std::string& fn, const pp::TranslateOptions& o = {}) {
    static std::vector<pp::ir::Module> keep;  // translations point into their module
    keep.push_back(pp::ir::parse_module(ir));
    const auto* f = keep.back().find(fn);
    REQUIRE(f != nullptr);
    return pp::translate(keep.back(), *f, o);
}

std::string verdict(const std::string& ir, const std::string& fn, const pp::TranslateOptions& o = {},
                    std::string* cls = nullptr) {
    auto t = tr(ir, fn, o);
    if (!t.fn) return "NT: " + t.reason;
    auto v = pp::check_function(*t.fn, 4, 30.0);
    if (cls) *cls = v.cls;
    if (v.status == prism::laws::NEEDS_HARNESS) return v.status + ": " + v.message;
    return v.status;
}

}  // namespace

TEST_CASE("pir3 fp: literals, formats and concrete IEEE semantics") {
    CHECK(fp::from_double_bits(dbits(1.5), 32) == fbits(1.5f));
    CHECK(fp::from_double_bits(dbits(0.1), 32) == std::nullopt);  // 0.1 is not a float
    CHECK(fp::from_double_bits(dbits(1.0), 16) == 0x3C00u);
    CHECK(fp::ll_const(fbits(1.5f), 32) == "0x3FF8000000000000");
    CHECK(fp::ll_const(0x3C00, 16) == "0xH3C00");
    CHECK(fp::format(dbits(-2.5), 64) == "-2.5");
    CHECK(fp::format(dbits(INFINITY), 64) == "inf");
    CHECK(fp::is_nan(0x7FC00000, 32));
    CHECK_FALSE(fp::is_nan(0x7F800000, 32));
    // half rounding: 2049 is halfway between 2048 and 2050 -> even (2048)
    CHECK(fp::from_double(2049.0, 16) == fp::from_double(2048.0, 16));
    CHECK(fp::from_double(65520.0, 16) == 0x7C00u);  // rounds to infinity
    CHECK(fp::from_double(std::ldexp(1.0, -24), 16) == 1u);  // smallest subnormal
    using pp::Op;
    std::vector<unsigned> w32{32, 32};
    CHECK(fp::eval(Op::FAdd, 32, {fbits(16777216.0f), fbits(1.0f)}, w32) == fbits(16777216.0f));
    CHECK(fp::eval(Op::FOlt, 1, {fbits(1.0f), fbits(2.0f)}, w32) == 1);
    CHECK(fp::eval(Op::FUno, 1, {0x7FC00000, fbits(2.0f)}, w32) == 1);
    CHECK(fp::eval(Op::FToSIOvf, 1, {dbits(2147483648.0), 32}, {64, 8}) == 1);
    CHECK(fp::eval(Op::FToSIOvf, 1, {dbits(-2147483648.9), 32}, {64, 8}) == 0);  // truncates to INT_MIN
    CHECK(fp::eval(Op::FToUIOvf, 1, {dbits(-0.9), 32}, {64, 8}) == 0);            // truncates to 0
    CHECK(fp::eval(Op::FToUIOvf, 1, {dbits(-1.0), 32}, {64, 8}) == 1);
    CHECK(fp::eval(Op::FRem, 64, {dbits(-7.0), dbits(2.0)}, {64, 64}) == dbits(-1.0));  // C fmod
    CHECK(fp::eval(Op::SIToF, 32, {16777217}, {32}) == fbits(16777216.0f));
    CHECK(fp::ambiguous(Op::FMinNum, 64, {dbits(0.0), dbits(-0.0)}, {64, 64}));
    CHECK_FALSE(fp::ambiguous(Op::FMinNum, 64, {dbits(1.0), dbits(-0.0)}, {64, 64}));
}

TEST_CASE("pir3 fp: parser keeps floating-point literals") {
    auto m = pp::ir::parse_module(R"IR(define double @f(double %x) {
entry:
  %a = fadd double %x, 1.500000e+00
  %b = fmul double %a, 0x3FE0000000000000
  %c = fcmp olt double %b, 0.000000e+00
  %d = select i1 %c, double 0.000000e+00, double %b
  ret double %d
}
)IR");
    const auto* f = m.find("f");
    REQUIRE(f);
    auto& in = f->blocks[0].insts;
    CHECK(in[0].ops[1].v.kind == pp::ir::Value::Fp);
    CHECK(in[0].ops[1].v.bits == dbits(1.5));
    CHECK(in[1].ops[1].v.bits == dbits(0.5));
    CHECK(in[2].pred == "olt");
}

#ifdef PRISM_HAS_Z3
TEST_CASE("pir3 fp: encoder, fptosi range check and interpreter agree") {
    const char* ir = R"IR(define i32 @conv(double %x) {
entry:
  %c = fcmp olt double %x, 1.000000e+03
  %d = fcmp ogt double %x, -1.000000e+03
  %a = and i1 %c, %d
  br i1 %a, label %in, label %out
in:
  %i = fptosi double %x to i32
  ret i32 %i
out:
  ret i32 0
}
define i32 @conv_bad(double %x) {
entry:
  %i = fptosi double %x to i32
  ret i32 %i
}
define float @fsum(float %a, float %b) {
entry:
  %s = fadd float %a, %b
  ret float %s
}
)IR";
    CHECK(verdict(ir, "conv") == prism::laws::PROVED);
    std::string cls;
    CHECK(verdict(ir, "conv_bad", {}, &cls) == prism::laws::FAILED);
    CHECK(cls == "FLOAT-CAST-OVF");
    auto t = tr(ir, "fsum");
    REQUIRE(t.fn);
    CHECK(t.fn->vars[static_cast<std::size_t>(t.fn->params[0])].fp);
    auto r = pp::interpret(*t.fn, {fbits(1.25f), fbits(2.5f)});
    CHECK(r.status == pp::InterpResult::Returned);
    CHECK(r.ret == fbits(3.75f));
    CHECK(pp::format_cex(*t.fn, {fbits(1.25f), fbits(2.5f)}) == "a=1.25, b=2.5");
}

TEST_CASE("pir3 fp: --fp-checks is opt-in") {
    const char* ir = R"IR(define double @div(double %a, double %b) {
entry:
  %q = fdiv double %a, %b
  ret double %q
}
)IR";
    CHECK(verdict(ir, "div") == prism::laws::PROVED);
    pp::TranslateOptions o;
    o.fp_checks = true;
    std::string cls;
    CHECK(verdict(ir, "div", o, &cls) == prism::laws::FAILED);
    CHECK((cls == "FLOAT-DIV-ZERO" || cls == "FLOAT-INVALID" || cls == "FLOAT-OVERFLOW"));
}

TEST_CASE("pir3 fp: fast-math flags and x86_fp80 stay unencoded") {
    const char* ir = R"IR(define double @fm(double %a) {
entry:
  %q = fadd fast double %a, 1.000000e+00
  ret double %q
}
define i32 @ld(x86_fp80 %a) {
entry:
  ret i32 0
}
)IR";
    CHECK(verdict(ir, "fm").find("fast-math") != std::string::npos);
    CHECK(verdict(ir, "ld").find("NT: UNENCODED") == 0);
}

TEST_CASE("pir3 indirect calls: function pointers dispatch over address-taken functions") {
    const char* ir = R"IR(define internal i32 @inc(i32 %v) {
entry:
  %r = add nsw i32 %v, 1
  ret i32 %r
}
define internal i32 @dec(i32 %v) {
entry:
  %r = sub nsw i32 %v, 1
  ret i32 %r
}
define i32 @pick(i32 %x) {
entry:
  %c = icmp sgt i32 %x, 0
  %f = select i1 %c, ptr @inc, ptr @dec
  %r = call i32 %f(i32 %x)
  ret i32 %r
}
define i32 @pick_ok(i32 %x) {
entry:
  %c = icmp sgt i32 %x, 0
  %f = select i1 %c, ptr @dec, ptr @inc
  %r = call i32 %f(i32 %x)
  ret i32 %r
}
define i32 @null_call(i32 %x) {
entry:
  %r = call i32 null(i32 %x)
  ret i32 %r
}
)IR";
    std::string cls;
    CHECK(verdict(ir, "pick", {}, &cls) == prism::laws::FAILED);  // inc(INT_MAX)
    CHECK(cls == "INT-SIGNED-OVF");
    CHECK(verdict(ir, "pick_ok") == prism::laws::PROVED);
}

TEST_CASE("pir3 exceptions: throw to a catch handler, noexcept escape") {
    const char* ir = R"IR(@_ZTVN10__cxxabiv117__class_type_infoE = external global [0 x ptr]
@_ZTS1E = linkonce_odr constant [3 x i8] c"1E\00"
@_ZTI1E = linkonce_odr constant { ptr, ptr } { ptr getelementptr inbounds (ptr, ptr @_ZTVN10__cxxabiv117__class_type_infoE, i64 2), ptr @_ZTS1E }

define internal i32 @thrower(i32 %x) {
entry:
  %cmp = icmp sgt i32 %x, 10
  br i1 %cmp, label %if.then, label %if.end
if.then:
  %e = call ptr @__cxa_allocate_exception(i64 4)
  store i32 %x, ptr %e, align 16
  call void @__cxa_throw(ptr %e, ptr @_ZTI1E, ptr null)
  unreachable
if.end:
  ret i32 %x
}

define i32 @catch_it(i32 %x) personality ptr @__gxx_personality_v0 {
entry:
  %call = invoke i32 @thrower(i32 %x)
          to label %ok unwind label %lpad
ok:
  br label %return
lpad:
  %i = landingpad { ptr, i32 }
          catch ptr @_ZTI1E
  %i1 = extractvalue { ptr, i32 } %i, 0
  %i2 = extractvalue { ptr, i32 } %i, 1
  %i3 = call i32 @llvm.eh.typeid.for(ptr @_ZTI1E)
  %m = icmp eq i32 %i2, %i3
  br i1 %m, label %catch, label %resume
catch:
  %p = call ptr @__cxa_begin_catch(ptr %i1)
  %v = load i32, ptr %p, align 4
  call void @__cxa_end_catch()
  %d = sdiv i32 100, %v
  br label %return
return:
  %r = phi i32 [ %call, %ok ], [ %d, %catch ]
  ret i32 %r
resume:
  resume { ptr, i32 } %i
}

define i32 @nothrow(i32 %x) personality ptr @__gxx_personality_v0 {
entry:
  %call = invoke i32 @thrower(i32 %x)
          to label %ok unwind label %tlpad
ok:
  ret i32 %call
tlpad:
  %i = landingpad { ptr, i32 }
          catch ptr null
  %i1 = extractvalue { ptr, i32 } %i, 0
  call void @__clang_call_terminate(ptr %i1)
  unreachable
}

declare ptr @__cxa_allocate_exception(i64)
declare void @__cxa_throw(ptr, ptr, ptr)
declare ptr @__cxa_begin_catch(ptr)
declare void @__cxa_end_catch()
declare i32 @llvm.eh.typeid.for(ptr)
declare i32 @__gxx_personality_v0(...)
declare void @__clang_call_terminate(ptr)
)IR";
    // the handler divides by the thrown value (> 10): no division by zero
    CHECK(verdict(ir, "catch_it") == prism::laws::PROVED);
    std::string cls;
    CHECK(verdict(ir, "nothrow", {}, &cls) == prism::laws::FAILED);
    CHECK(cls == "CXX-THROW-NOEXCEPT");
}

TEST_CASE("pir3 inline assembly: NEEDS-HARNESS without a contract, assumed with one") {
    const char* ir = R"IR(define i32 @a(i32 %x) {
entry:
  %r = call i32 asm "movl $1, $0", "=r,r"(i32 %x), !dbg !7
  %d = sdiv i32 100, %r
  ret i32 %d
}
!7 = !DILocation(line: 5, column: 3, scope: !8)
)IR";
    auto no = tr(ir, "a");
    CHECK_FALSE(no.fn);
    CHECK(no.reason.find("inline assembly") != std::string::npos);
    pp::TranslateOptions o;
    o.asm_contracts[5] = "r > 0 && r <= 10";
    auto t = tr(ir, "a", o);
    REQUIRE(t.fn);
    REQUIRE(t.fn->assumptions.size() == 1);
    CHECK(t.fn->assumptions[0].find("r > 0 && r <= 10") != std::string::npos);
    CHECK(pp::check_function(*t.fn, 4, 30.0).status == prism::laws::PROVED);
    o.asm_contracts[5] = "r >= 0";
    CHECK(verdict(ir, "a", o) == prism::laws::FAILED);  // r == 0 allowed by the contract
    o.asm_contracts[5] = "r is small";
    CHECK(verdict(ir, "a", o).find("not understood") != std::string::npos);
}

TEST_CASE("pir nondet: a refutation reports the nondet values and call sites its path reads, in call order") {
    // SV-COMP replay and witnesses (tools/svcomp): the same "fn=value, ..." as
    // the bmc stage, signed per C type, only calls that run before the
    // violated check, and each call's debug location in extra["nondet_loc"].
    const char* ir = R"IR(define i32 @main() {
entry:
  %a = call i32 @__VERIFIER_nondet_int(), !dbg !1
  %neg = icmp slt i32 %a, 0
  br i1 %neg, label %skip, label %go
skip:
  %c = call signext i8 @__VERIFIER_nondet_char(), !dbg !2
  ret i32 0
go:
  %k = call zeroext i8 @__VERIFIER_nondet_uchar(), !dbg !3
  %big = icmp sgt i32 %a, 2147483000
  %k200 = icmp eq i8 %k, 200
  %both = and i1 %big, %k200
  br i1 %both, label %ovf, label %done
ovf:
  %b = add nsw i32 %a, 1000
  %z = call i32 @__VERIFIER_nondet_int(), !dbg !4
  %r = add i32 %b, %z
  ret i32 %r
done:
  ret i32 0
}
define i32 @neg() {
entry:
  %c = call signext i8 @__VERIFIER_nondet_char(), !dbg !5
  %lo = icmp slt i8 %c, -100
  br i1 %lo, label %bad, label %ok
bad:
  call void @reach_error(), !dbg !6
  unreachable
ok:
  ret i32 0
}
define i32 @pure(i32 %x) {
entry:
  %y = add nsw i32 %x, 1
  ret i32 %y
}
declare i32 @__VERIFIER_nondet_int()
declare signext i8 @__VERIFIER_nondet_char()
declare zeroext i8 @__VERIFIER_nondet_uchar()
declare void @reach_error()
!1 = !DILocation(line: 5, column: 11, scope: !9)
!2 = !DILocation(line: 6, column: 25, scope: !9)
!3 = !DILocation(line: 7, column: 21, scope: !9)
!4 = !DILocation(line: 8, column: 13, scope: !9)
!5 = !DILocation(line: 12, column: 12, scope: !9)
!6 = !DILocation(line: 13, column: 20, scope: !9)
)IR";
    auto t = tr(ir, "main");
    REQUIRE(t.fn);
    auto v = pp::check_function(*t.fn, 4, 30.0);
    REQUIRE(v.status == std::string(prism::laws::FAILED));
    CHECK(v.cls == "INT-SIGNED-OVF");
    REQUIRE(v.extra.count("nondet"));
    auto nd = v.extra.at("nondet");
    // the char call is on the branch the violating path skips; the second
    // int call runs after the violated check
    CHECK(nd.find("__VERIFIER_nondet_char") == std::string::npos);
    const std::string pre = "__VERIFIER_nondet_int=";
    REQUIRE(nd.rfind(pre, 0) == 0);
    auto comma = nd.find(", ");
    REQUIRE(comma != std::string::npos);
    CHECK(std::stoll(nd.substr(pre.size(), comma - pre.size())) > 2147483000);
    CHECK(nd.substr(comma + 2) == "__VERIFIER_nondet_uchar=200");  // unsigned char: 200, not -56
    CHECK(v.extra.at("nondet_loc") == "5:11, 7:21");

    auto tn = tr(ir, "neg");
    REQUIRE(tn.fn);
    auto vn = pp::check_function(*tn.fn, 4, 30.0);
    REQUIRE(vn.status == std::string(prism::laws::FAILED));
    REQUIRE(vn.extra.count("nondet"));
    const std::string cpre = "__VERIFIER_nondet_char=";
    REQUIRE(vn.extra.at("nondet").rfind(cpre, 0) == 0);
    auto cv = std::stoll(vn.extra.at("nondet").substr(cpre.size()));  // signed char: negative
    CHECK(cv < -100);
    CHECK(cv >= -128);
    CHECK(vn.extra.at("nondet_loc") == "12:12");

    // no nondet call: no nondet key
    auto tp = tr(ir, "pure");
    REQUIRE(tp.fn);
    auto vp = pp::check_function(*tp.fn, 4, 30.0);
    CHECK(vp.status == std::string(prism::laws::FAILED));
    CHECK_FALSE(vp.extra.count("nondet"));
}
#endif

TEST_CASE("pir3 inline assembly: contract comments map to the asm line") {
    std::vector<std::string> src{"int f(int x) {", "  int r;", "  // prism: asm ensures r >= 0",
                                 "  __asm__(\"...\" : \"=r\"(r));", "  return r;", "}"};
    auto m = pp::pirmem::asm_contracts(src);
    REQUIRE(m.count(4));
    CHECK(m[4] == "r >= 0");
}

TEST_CASE("pir3 setjmp: locals of a setjmp function stay in memory") {
    std::string ir = "define i32 @f() {\nentry:\n  %b = alloca [1 x i64], align 16\n  %x = alloca i32, align 4\n"
                     "  %r = call i32 @_setjmp(ptr %b)\n  ret i32 %r\n}\n"
                     "define i32 @g() {\nentry:\n  %y = alloca i32, align 4\n  ret i32 0\n}\n";
    auto out = pp::pirctl::keep_setjmp_locals(ir);
    CHECK(out.find("call void @__prism.keep(ptr %b)") != std::string::npos);
    CHECK(out.find("call void @__prism.keep(ptr %x)") != std::string::npos);
    CHECK(out.find("@__prism.keep(ptr %y)") == std::string::npos);
    CHECK(out.find("declare void @__prism.keep(ptr)") != std::string::npos);
}

TEST_CASE("pir3 debug dump (PIR3_DUMP=file.ll:function, developer aid)") {
    const char* spec = std::getenv("PIR3_DUMP");
    if (!spec) return;
    std::string s(spec);
    auto colon = s.rfind(':');
    REQUIRE(colon != std::string::npos);
    std::ifstream in(s.substr(0, colon));
    std::stringstream ss;
    ss << in.rdbuf();
    static std::vector<pp::ir::Module> keep;
    keep.push_back(pp::ir::parse_module(ss.str()));
    auto lib = pp::build_models(pp::find_frontend(prism::Config{}), 60.0);
    pp::link_models(keep.back(), lib);
    const auto* f = keep.back().find(s.substr(colon + 1));
    REQUIRE(f);
    pp::TranslateOptions o;
    o.inline_depth = 12;
    auto t = pp::translate(keep.back(), *f, o);
    MESSAGE((t.fn ? pp::to_text(*t.fn) : t.reason));
    if (t.fn) {
        auto v = pp::check_function(*t.fn, 8, 60.0);
        MESSAGE((v.status + ": " + v.message));
    }
}
