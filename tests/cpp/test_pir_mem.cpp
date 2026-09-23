// Doctests: PIR memory model, library models and pointer contracts
// (docs/PIR.md "Memory model", "Library models", "Pointer parameters").
// Linked into prism_tests next to test_main.cpp (which provides main()).
#include <doctest/doctest.h>
#ifdef ERROR
#  undef ERROR
#endif

#include "prism/config.hpp"
#include "prism/laws.hpp"
#include "prism/pir.hpp"

#include "../../src/prism/pir/translate_mem.hpp"

#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

namespace {

namespace pp = prism::pir;

pp::Translation mem_tr(const std::string& ir, const std::string& fn, const pp::TranslateOptions& o = {}) {
    auto m = pp::ir::parse_module(ir);
    const auto* f = m.find(fn);
    REQUIRE(f != nullptr);
    return pp::translate(m, *f, o);
}

const char* kDecls = R"IR(
declare ptr @__prism_alloc(i64, i32, i32)
declare void @__prism_free(ptr, i32)
declare void @llvm.memset.p0.i64(ptr, i8, i64, i1)
declare void @llvm.memcpy.p0.p0.i64(ptr, ptr, i64, i1)
declare void @reach_error()
)IR";

std::string with_decls(const std::string& body) { return body + kDecls; }

struct Case {
    const char* name;
    const char* ir;
    const char* status;  // expected status
    const char* cls;     // expected class ("" = any / none)
};

// Hand-written post-mem2reg IR, one property each (true and false variants).
const std::vector<Case>& cases() {
    static const std::vector<Case> k{
        {"arr_ok", R"IR(define i32 @arr_ok(i32 %i) {
entry:
  %a = alloca [4 x i32], align 16
  call void @llvm.memset.p0.i64(ptr align 16 %a, i8 0, i64 16, i1 false)
  %m = and i32 %i, 3
  %x = sext i32 %m to i64
  %p = getelementptr inbounds [4 x i32], ptr %a, i64 0, i64 %x
  %v = load i32, ptr %p, align 4
  ret i32 %v
}
)IR", "PROVED", ""},
        {"arr_bad", R"IR(define i32 @arr_bad(i32 %i) {
entry:
  %a = alloca [4 x i32], align 16
  call void @llvm.memset.p0.i64(ptr align 16 %a, i8 0, i64 16, i1 false)
  %m = and i32 %i, 4
  %x = sext i32 %m to i64
  %p = getelementptr inbounds [4 x i32], ptr %a, i64 0, i64 %x
  %v = load i32, ptr %p, align 4
  ret i32 %v
}
)IR", "FAILED", "MEM-OOB-READ"},
        {"arith_bad", R"IR(define i32 @arith_bad(i32 %i) {
entry:
  %a = alloca [4 x i32], align 16
  %m = and i32 %i, 7
  %x = sext i32 %m to i64
  %p = getelementptr inbounds i32, ptr %a, i64 %x
  %c = icmp eq ptr %p, %a
  %r = zext i1 %c to i32
  ret i32 %r
}
)IR", "FAILED", "MEM-PTR-ARITH"},
        {"uaf", R"IR(define i32 @uaf(i32 %c) {
entry:
  %p = call ptr @__prism_alloc(i64 4, i32 2, i32 1)
  call void @__prism_free(ptr %p, i32 2)
  %t = icmp ne i32 %c, 0
  br i1 %t, label %use, label %done
use:
  %v = load i32, ptr %p, align 4
  ret i32 %v
done:
  ret i32 0
}
)IR", "FAILED", "MEM-UAF"},
        {"free_ok", R"IR(define i32 @free_ok(i32 %c) {
entry:
  %p = call ptr @__prism_alloc(i64 4, i32 2, i32 1)
  %v = load i32, ptr %p, align 4
  call void @__prism_free(ptr %p, i32 2)
  call void @__prism_free(ptr null, i32 2)
  ret i32 %v
}
)IR", "PROVED", ""},
        {"double_free", R"IR(define i32 @double_free(i32 %c) {
entry:
  %p = call ptr @__prism_alloc(i64 4, i32 2, i32 0)
  call void @__prism_free(ptr %p, i32 2)
  call void @__prism_free(ptr %p, i32 2)
  ret i32 0
}
)IR", "FAILED", "MEM-DOUBLE-FREE"},
        {"invalid_free", R"IR(define i32 @invalid_free(i32 %c) {
entry:
  %x = alloca i32, align 4
  call void @__prism_free(ptr %x, i32 2)
  ret i32 0
}
)IR", "FAILED", "MEM-INVALID-FREE"},
        {"free_offset", R"IR(define i32 @free_offset(i32 %c) {
entry:
  %p = call ptr @__prism_alloc(i64 8, i32 2, i32 0)
  %q = getelementptr inbounds i8, ptr %p, i64 1
  call void @__prism_free(ptr %q, i32 2)
  ret i32 0
}
)IR", "FAILED", "MEM-INVALID-FREE"},
        {"mismatch", R"IR(define i32 @mismatch(i32 %c) {
entry:
  %p = call ptr @__prism_alloc(i64 16, i32 6, i32 0)
  call void @__prism_free(ptr %p, i32 5)
  ret i32 0
}
)IR", "FAILED", "MEM-MISMATCHED-FREE"},
        {"null_deref", R"IR(define i32 @null_deref(i32 %c) {
entry:
  %x = alloca i32, align 4
  store i32 1, ptr %x, align 4
  %t = icmp ne i32 %c, 0
  %p = select i1 %t, ptr %x, ptr null
  %v = load i32, ptr %p, align 4
  ret i32 %v
}
)IR", "FAILED", "PTR-NULL-DEREF"},
        {"misaligned", R"IR(define i32 @misaligned(i32 %c) {
entry:
  %b = alloca [8 x i8], align 8
  call void @llvm.memset.p0.i64(ptr align 8 %b, i8 0, i64 8, i1 false)
  %p = getelementptr inbounds i8, ptr %b, i64 2
  %v = load i32, ptr %p, align 4
  ret i32 %v
}
)IR", "FAILED", "MEM-MISALIGNED"},
        {"aligned", R"IR(define i32 @aligned(i32 %c) {
entry:
  %b = alloca [8 x i8], align 8
  call void @llvm.memset.p0.i64(ptr align 8 %b, i8 0, i64 8, i1 false)
  %p = getelementptr inbounds i8, ptr %b, i64 4
  %v = load i32, ptr %p, align 4
  ret i32 %v
}
)IR", "PROVED", ""},
        {"uninit_mem", R"IR(define i32 @uninit_mem(i32 %c) {
entry:
  %a = alloca [2 x i32], align 4
  store i32 1, ptr %a, align 4
  %m = and i32 %c, 1
  %x = sext i32 %m to i64
  %p = getelementptr inbounds [2 x i32], ptr %a, i64 0, i64 %x
  %v = load i32, ptr %p, align 4
  ret i32 %v
}
)IR", "FAILED", "UNINIT-READ"},
        {"ptr_cmp", R"IR(define i32 @ptr_cmp(i32 %c) {
entry:
  %a = alloca i32, align 4
  %b = alloca i32, align 4
  %t = icmp ult ptr %a, %b
  %r = zext i1 %t to i32
  ret i32 %r
}
)IR", "FAILED", "PTR-COMPARE"},
        {"overlap", R"IR(define i32 @overlap(i32 %c) {
entry:
  %a = alloca [8 x i8], align 1
  call void @llvm.memset.p0.i64(ptr %a, i8 0, i64 8, i1 false)
  %d = getelementptr inbounds i8, ptr %a, i64 1
  call void @llvm.memcpy.p0.p0.i64(ptr %d, ptr %a, i64 4, i1 false)
  ret i32 0
}
)IR", "FAILED", "MEM-OVERLAP"},
        {"copy_ok", R"IR(define i32 @copy_ok(i32 %c) {
entry:
  %a = alloca [4 x i8], align 1
  %b = alloca [4 x i8], align 1
  call void @llvm.memset.p0.i64(ptr %a, i8 7, i64 4, i1 false)
  call void @llvm.memcpy.p0.p0.i64(ptr %b, ptr %a, i64 4, i1 false)
  %m = and i32 %c, 3
  %x = sext i32 %m to i64
  %p = getelementptr inbounds i8, ptr %b, i64 %x
  %v = load i8, ptr %p, align 1
  %e = icmp eq i8 %v, 7
  br i1 %e, label %ok, label %bad
bad:
  call void @reach_error()
  unreachable
ok:
  ret i32 0
}
)IR", "PROVED", ""},
        {"literal_write", R"IR(@.s = private unnamed_addr constant [4 x i8] c"abc\00", align 1
define i32 @literal_write(i32 %c) {
entry:
  store i8 120, ptr @.s, align 1
  ret i32 0
}
)IR", "FAILED", "MEM-WRITE-CONST"},
        {"literal_read", R"IR(@.s = private unnamed_addr constant [4 x i8] c"abc\00", align 1
define i32 @literal_read(i32 %c) {
entry:
  %v = load i8, ptr getelementptr inbounds ([4 x i8], ptr @.s, i64 0, i64 2), align 1
  %e = icmp eq i8 %v, 99
  br i1 %e, label %ok, label %bad
bad:
  call void @reach_error()
  unreachable
ok:
  ret i32 0
}
)IR", "PROVED", ""},
        {"vla_size", R"IR(define i32 @vla_size(i32 %n) {
entry:
  %z = zext i32 %n to i64
  %s = call ptr @llvm.stacksave.p0()
  %v = alloca i32, i64 %z, align 16
  store i32 1, ptr %v, align 16
  call void @llvm.stackrestore.p0(ptr %s)
  ret i32 0
}
declare ptr @llvm.stacksave.p0()
declare void @llvm.stackrestore.p0(ptr)
)IR", "FAILED", "MEM-VLA-SIZE"},
        {"struct_ok", R"IR(%struct.S = type { i32, i8, i64 }
define i32 @struct_ok(i32 %c) {
entry:
  %s = alloca %struct.S, align 8
  %c2 = getelementptr inbounds %struct.S, ptr %s, i32 0, i32 2
  store i64 3, ptr %c2, align 8
  %v = load i64, ptr %c2, align 8
  %t = trunc i64 %v to i32
  ret i32 %t
}
)IR", "PROVED", ""},
    };
    return k;
}

}  // namespace

TEST_CASE("pir mem: IR parser reads memory instructions, globals and named types") {
    const char* ir = R"IR(
target datalayout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
%struct.S = type { i32, i8, i64 }
@g = dso_local global i32 5, align 4
@arr = dso_local global [3 x i32] [i32 1, i32 2, i32 3], align 4
@.str = private unnamed_addr constant [3 x i8] c"hi\00", align 1
@p = dso_local global ptr @.str, align 8
@stdout = external global ptr, align 8
define i32 @f(i64 %n) {
entry:
  %a = alloca [4 x i32], align 16
  %v = alloca i32, i64 %n, align 16
  %q = getelementptr inbounds [4 x i32], ptr %a, i64 0, i64 1
  store i32 7, ptr %q, align 4
  %x = load i32, ptr getelementptr inbounds ([3 x i32], ptr @arr, i64 0, i64 1), align 4
  ret i32 %x
}
)IR";
    auto m = pp::ir::parse_module(ir);
    CHECK(m.datalayout.find("i64:64") != std::string::npos);
    REQUIRE(m.types.count("struct.S"));
    CHECK(m.types["struct.S"].elems.size() == 3);
    REQUIRE(m.globals.size() == 5);
    auto* g = m.find_global("g");
    REQUIRE(g);
    CHECK_FALSE(g->is_const);
    CHECK(g->init.at(0).v.bits == 5);
    auto* arr = m.find_global("arr");
    REQUIRE(arr);
    CHECK(arr->init.at(0).v.kind == pp::ir::Value::Aggregate);
    CHECK(arr->init.at(0).v.elems.size() == 3);
    auto* s = m.find_global(".str");
    REQUIRE(s);
    CHECK(s->is_const);
    CHECK(s->init.at(0).v.kind == pp::ir::Value::Str);
    CHECK(s->init.at(0).v.bytes == std::string("hi\0", 3));
    CHECK(m.find_global("p")->init.at(0).v.kind == pp::ir::Value::Global);
    CHECK(m.find_global("stdout")->external);
    auto& b = m.functions.at(0).blocks.at(0).insts;
    REQUIRE(b.size() == 6);
    CHECK(b[0].op == "alloca");
    CHECK(b[0].ety.text == "[4 x i32]");
    CHECK(b[0].align == 16);
    CHECK(b[1].ops.size() == 1);  // dynamic count
    CHECK(b[2].op == "getelementptr");
    CHECK(b[2].ety.text == "[4 x i32]");
    CHECK(b[2].ops.size() == 3);
    CHECK(b[3].op == "store");
    CHECK(b[3].ops.size() == 2);
    CHECK(b[4].ops.at(0).v.kind == pp::ir::Value::ConstExpr);
    CHECK(b[4].ops.at(0).v.ce_op == "getelementptr");
    CHECK(b[4].ops.at(0).v.elems.size() == 3);
}

TEST_CASE("pir mem: data layout sizes and struct offsets") {
    auto m = pp::ir::parse_module("%struct.S = type { i32, i8, i64 }\n%struct.P = type <{ i8, i32 }>\n");
    pp::pirmem::Layout lay(m);
    auto S = pp::ir::parse_type("%struct.S");
    CHECK(lay.alloc_size(S) == 16);
    CHECK(lay.field_offset(S, 1) == 4);
    CHECK(lay.field_offset(S, 2) == 8);
    CHECK(lay.align(S) == 8);
    auto P = pp::ir::parse_type("%struct.P");
    CHECK(lay.alloc_size(P) == 5);
    CHECK(lay.field_offset(P, 1) == 1);
    CHECK(lay.alloc_size(pp::ir::parse_type("[3 x i16]")) == 6);
    CHECK(lay.alloc_size(pp::ir::parse_type("ptr")) == 8);
    CHECK(lay.store_size(pp::ir::parse_type("i1")) == 1);
}

#ifdef PRISM_HAS_Z3
TEST_CASE("pir mem: every property, true and false, in both memory encodings") {
    for (auto& c : cases()) {
        CAPTURE(c.name);
        auto t = mem_tr(with_decls(c.ir), c.name);
        REQUIRE_MESSAGE(t.fn.has_value(), t.reason);
        CHECK(t.fn->uses_memory);
        for (auto enc : {pp::MemEncoding::Array, pp::MemEncoding::Bv}) {
            pp::EncodeOptions eo;
            eo.memory = enc;
            auto v = pp::check_function(*t.fn, 8, 30, eo);
            CAPTURE(v.message);
            CHECK(v.status == c.status);
            if (*c.cls) CHECK(v.cls == c.cls);
            if (v.status == prism::laws::FAILED && !v.cex_args.empty()) {
                // the interpreter reproduces the violation on the counterexample
                auto r = pp::interpret(*t.fn, v.cex_args);
                CHECK(r.status == pp::InterpResult::Violation);
            }
        }
    }
}

TEST_CASE("pir mem: VCs of the Bv encoding are QF_BV (no arrays)") {
    auto t = mem_tr(with_decls(cases()[1].ir), "arr_bad");
    REQUIRE(t.fn.has_value());
    auto vcs = pp::pir_vcs(*t.fn, 8);
    REQUIRE_FALSE(vcs.empty());
    for (auto& vc : vcs) CHECK(vc.smt2.find("Array") == std::string::npos);
    pp::EncodeOptions eo;
    eo.memory = pp::MemEncoding::Array;
    auto avc = pp::pir_vcs(*t.fn, 8, eo);
    REQUIRE_FALSE(avc.empty());
}

TEST_CASE("pir mem: interpreter runs memory code concretely") {
    auto t = mem_tr(with_decls(cases()[0].ir), "arr_ok");
    REQUIRE(t.fn.has_value());
    auto r = pp::interpret(*t.fn, {6});
    CHECK(r.status == pp::InterpResult::Returned);
    CHECK(r.ret == 0);
    auto bad = mem_tr(with_decls(cases()[1].ir), "arr_bad");
    auto rb = pp::interpret(*bad.fn, {4});
    CHECK(rb.status == pp::InterpResult::Violation);
    CHECK(rb.cls == "MEM-OOB-READ");
}

TEST_CASE("pir mem: mutable globals are arbitrary except at program entry") {
    const char* ir = R"IR(@g = dso_local global i32 5, align 4
define i32 @reads_g(i32 %c) {
entry:
  %v = load i32, ptr @g, align 4
  %e = icmp eq i32 %v, 5
  br i1 %e, label %ok, label %bad
bad:
  call void @reach_error()
  unreachable
ok:
  ret i32 0
}
)IR";
    auto t = mem_tr(with_decls(ir), "reads_g");
    REQUIRE(t.fn.has_value());
    CHECK(t.fn->mutable_globals);
    CHECK(pp::check_function(*t.fn, 8, 30).status == prism::laws::FAILED);
    pp::TranslateOptions o;
    o.globals_initial = true;
    auto t2 = mem_tr(with_decls(ir), "reads_g", o);
    CHECK(pp::check_function(*t2.fn, 8, 30).status == prism::laws::PROVED);
}

TEST_CASE("pir mem: pointer contracts bind a fresh object (Law 6 relaxed with assumptions)") {
    const char* ir = R"IR(define i32 @f(ptr %p, i32 %n) {
entry:
  %x = getelementptr inbounds i32, ptr %p, i64 3
  %v = load i32, ptr %x, align 4
  ret i32 %v
}
)IR";
    auto none = mem_tr(ir, "f");
    CHECK_FALSE(none.fn.has_value());
    CHECK(none.reason.find("Law 6") != std::string::npos);
    auto m = pp::ir::parse_module(ir);
    std::vector<std::string> src{"// requires: \\valid(p + (0..3))", "int f(int *p, int n) {", "  return p[3];", "}"};
    std::vector<std::string> unparsed;
    auto cs = pp::parse_contracts(src, 2, *m.find("f"), &unparsed);
    REQUIRE(cs.size() == 1);
    CHECK(cs[0].param == "p");
    CHECK(cs[0].count == 4);
    pp::TranslateOptions o;
    o.contracts = cs;
    auto t = pp::translate(m, *m.find("f"), o);
    REQUIRE_MESSAGE(t.fn.has_value(), t.reason);
    CHECK_FALSE(t.fn->assumptions.empty());
    CHECK(t.fn->ptr_params == std::vector<std::string>{"p"});
    CHECK(pp::check_function(*t.fn, 8, 30).status == prism::laws::PROVED);  // stage: PROVED-ASSUMING
    // symbolic size from another parameter: 0..n-1 elements
    std::vector<std::string> src2{"/*@ requires \\valid(p + (0..n-1)); */", "int f(int *p, int n) {"};
    auto cs2 = pp::parse_contracts(src2, 2, *m.find("f"));
    REQUIRE(cs2.size() == 1);
    CHECK(cs2[0].count_param == "n");
    CHECK(cs2[0].count_add == 0);
    o.contracts = cs2;
    auto t2 = pp::translate(m, *m.find("f"), o);
    REQUIRE(t2.fn.has_value());
    auto v2 = pp::check_function(*t2.fn, 8, 30);
    CHECK(v2.status == prism::laws::FAILED);  // n < 4 makes p[3] out of bounds
    CHECK((v2.cls == "MEM-OOB-READ" || v2.cls == "MEM-PTR-ARITH"));
}

TEST_CASE("pir mem: printf family format plans") {
    using pp::ir::parse_type;
    auto i32 = parse_type("i32"), ptr = parse_type("ptr"), i64 = parse_type("i64"), dbl = parse_type("double");
    auto ok = pp::pirmem::plan_format("printf", std::string("%d %s %ld %f\n"), {ptr, i32, ptr, i64, dbl});
    CHECK(ok.handled);
    CHECK(ok.unencoded.empty());
    CHECK(ok.violations.empty());
    CHECK(ok.cstr_args == std::vector<std::size_t>{2});
    auto n = pp::pirmem::plan_format("printf", std::string("x%n"), {ptr, ptr});
    REQUIRE(n.violations.size() == 1);
    CHECK(n.violations[0].first == "FMT-PERCENT-N");
    auto miss = pp::pirmem::plan_format("printf", std::string("%d %d"), {ptr, i32});
    REQUIRE(miss.violations.size() == 1);
    CHECK(miss.violations[0].first == "FMT-ARGS");
    auto ty = pp::pirmem::plan_format("printf", std::string("%s"), {ptr, i32});
    REQUIRE(ty.violations.size() == 1);
    CHECK(ty.violations[0].first == "FMT-ARGS");
    auto nonlit = pp::pirmem::plan_format("printf", std::nullopt, {ptr});
    CHECK(nonlit.unencoded.rfind("UNENCODED", 0) == 0);
    auto sp = pp::pirmem::plan_format("sprintf", std::string("v=%d"), {ptr, ptr, i32});
    CHECK(sp.buf_arg == 0);
    CHECK(sp.max_len == 13);
    CHECK_FALSE(pp::pirmem::plan_format("puts", std::string("x"), {ptr}).handled);
}

TEST_CASE("pir mem: library models are embedded, build with clang and link on demand") {
    auto& src = pp::model_sources();
    std::map<std::string, std::string> by;
    for (auto& [n, t] : src) by[n] = t;
    REQUIRE(by.count("string.c"));
    REQUIRE(by.count("stdlib.c"));
    CHECK(by["string.c"].find("size_t strlen(") != std::string::npos);
    auto cfg = prism::default_config();
    auto fe = pp::find_frontend(cfg);
    if (!fe.clang || !fe.opt) {
        MESSAGE("clang/opt not on PATH: model build skipped");
        return;
    }
    auto lib = pp::build_models(fe, 60);
    REQUIRE_MESSAGE(lib.error.empty(), lib.error);
    auto m = pp::ir::parse_module(R"IR(define i64 @f() {
entry:
  %n = call i64 @strdup_len()
  ret i64 %n
}
declare ptr @strdup(ptr)
declare i64 @strdup_len()
)IR");
    auto linked = pp::link_models(m, lib);
    bool strdup = false, strlen = false, malloc = false;
    for (auto& n : linked) {
        strdup = strdup || n == "strdup";
        strlen = strlen || n == "strlen";
        malloc = malloc || n == "malloc";
    }
    CHECK(strdup);
    CHECK(strlen);  // transitively: strdup calls strlen and malloc
    CHECK(malloc);
    REQUIRE(m.find("strlen"));
    CHECK(m.find("strlen")->is_model);
}

TEST_CASE("pir mem: clang round trip on tests/pir/mem_*.c (skips without clang/opt)") {
    auto cfg = prism::default_config();
    auto fe = pp::find_frontend(cfg);
    if (!fe.clang || !fe.opt) {
        MESSAGE("clang/opt not on PATH: pir memory round trip skipped");
        return;
    }
    auto dir = std::filesystem::path(__FILE__).parent_path().parent_path() / "pir";
    cfg.root = dir;
    cfg.jobs = 2;
    auto out = pp::run_pir({dir / "mem_array.c", dir / "mem_heap.c", dir / "mem_contract.c"}, cfg);
    std::map<std::string, std::pair<std::string, std::string>> st;
    for (auto& f : out)
        if (f.function) st[*f.function] = {f.status, f.cls};
    CHECK(st["arr_read_ok"].first == prism::laws::PROVED);
    CHECK(st["arr_read_bad"].first == prism::laws::FAILED);
    CHECK(st["arr_read_bad"].second == "MEM-OOB-READ");
    CHECK(st["uaf_bad"].second == "MEM-UAF");
    CHECK(st["double_free_bad"].second == "MEM-DOUBLE-FREE");
    CHECK(st["null_bad"].second == "PTR-NULL-DEREF");
    CHECK(st["heap_ok"].first == prism::laws::PROVED);
    CHECK(st["no_contract"].first == prism::laws::NEEDS_HARNESS);
    CHECK(st["first_last_ok"].first == prism::laws::PROVED_ASSUMING);
    CHECK(st["past_end_bad"].first == prism::laws::FAILED);
}
// docs/CONFORMANCE.md F8/F9: one program per file (main sees the static
// initial state; each function's verdict is its own).
static std::map<std::string, std::pair<std::string, std::string>> pir_programs(
    const std::vector<std::pair<std::string, std::string>>& files) {
    auto cfg = prism::default_config();
    auto dir = std::filesystem::temp_directory_path() / "prism_pir_f8_f9";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    std::vector<std::filesystem::path> paths;
    for (auto& [name, src] : files) {
        std::ofstream(dir / name) << src;
        paths.push_back(dir / name);
    }
    cfg.root = dir;
    cfg.jobs = 2;
    cfg.solver_cache = dir / "cache";
    std::map<std::string, std::pair<std::string, std::string>> st;
    for (auto& f : pp::run_pir(paths, cfg))
        if (f.function) st[std::filesystem::path(f.file).filename().string() + ":" + *f.function] = {f.status, f.cls};
    return st;
}

TEST_CASE("pir mem: large zero-initialised globals keep their contents (F8; skips without clang/opt)") {
    auto fe = pp::find_frontend(prism::default_config());
    if (!fe.clang || !fe.opt) return;
    std::string dense;
    for (int i = 0; i < 300; ++i) dense += "1,";
    auto st = pir_programs({
        {"f8_true.c", "#include <assert.h>\nstruct uint3 { unsigned x, y, z; };\nstruct uint3 t[1024];\n"
                      "int main(void) { assert(t[0].x == 0 && t[1023].z == 0); return 0; }\n"},
        {"f8_false.c", "#include <assert.h>\nstruct uint3 { unsigned x, y, z; };\nstruct uint3 t[1024];\n"
                       "int main(void) { assert(t[1000].z == 1); return 0; }\n"},
        // a large table with many non-zero entries is still arbitrary bytes:
        // never a proof that depends on its contents
        {"f8_dense.c", "#include <assert.h>\nstatic int d[2048] = {" + dense +
                           "};\nint main(void) { assert(d[5] == 1); return 0; }\n"},
    });
    CHECK(st["f8_true.c:main"].first == prism::laws::PROVED);
    CHECK(st["f8_false.c:main"].first == prism::laws::FAILED);
    CHECK_FALSE(prism::laws::is_proof(st["f8_dense.c:main"].first));
}

TEST_CASE("pir mem: abort() after a failed allocation is OOM handling, not a defect (F9; skips without clang/opt)") {
    auto fe = pp::find_frontend(prism::default_config());
    if (!fe.clang || !fe.opt) return;
    auto st = pir_programs({{"f9.c", R"(#include <stdlib.h>
#include <assert.h>
int oom_abort(int n) { int *p = malloc(sizeof *p); if (!p) abort(); *p = n; int r = *p; free(p); return r; }
int oom_calloc(int n) { int *p = calloc(4, sizeof *p); if (!p) abort(); int r = p[n & 3]; free(p); return r; }
int plain_abort(int n) { int *p = malloc(sizeof *p); if (!p) abort(); if (n == 7) abort(); free(p); return 0; }
int abort_first(int n) { if (n == 3) abort(); int *p = malloc(sizeof *p); if (!p) abort(); free(p); return 0; }
int unchecked(int n) { int *p = malloc(sizeof *p); *p = n; int r = *p; free(p); return r; }
int assert_nonnull(int n) { int *p = malloc(sizeof *p); assert(p); *p = n; free(p); return 0; }
)"}});
    CHECK(st["f9.c:oom_abort"].first == prism::laws::PROVED);
    CHECK(st["f9.c:oom_calloc"].first == prism::laws::PROVED);
    // abort() on an execution where every allocation succeeded is still reported
    CHECK(st["f9.c:plain_abort"].first == prism::laws::FAILED);
    CHECK(st["f9.c:abort_first"].first == prism::laws::FAILED);
    // the allocation-failure checks themselves stay
    CHECK(st["f9.c:unchecked"].first == prism::laws::FAILED);
    CHECK(st["f9.c:unchecked"].second == "PTR-NULL-DEREF");
    CHECK(st["f9.c:assert_nonnull"].first == prism::laws::FAILED);
    CHECK(st["f9.c:assert_nonnull"].second == "FUNC-CONTRACT");
}

// k-induction for loops that write memory (docs/PIR.md "k-induction with
// memory"). Every function returns early for n < 20, so no path of the
// unwind-8 unrolling gets past the loop: the verdict after the loop rests on
// the k-induction step alone, which havocs the loop's write footprint.
namespace kind_mem {

// entry: two 16-byte arrays %a, %c (both set to `fill`), n < 20 returns;
// loop: while (n != 0) { BODY; n-- }; after: AFTER; ret %r.
std::string loop_fn(const std::string& name, const std::string& fill, const std::string& body,
                    const std::string& after) {
    return "define i32 @" + name + R"IR((i32 %n, i32 %j, i1 %s) {
entry:
  %a = alloca [4 x i32], align 16
  %c = alloca [4 x i32], align 16
)IR" + fill + R"IR(  %small = icmp ult i32 %n, 20
  br i1 %small, label %early, label %head
early:
  ret i32 0
head:
  %i = phi i32 [ %n, %entry ], [ %dec, %body ]
  %go = icmp ne i32 %i, 0
  br i1 %go, label %body, label %done
body:
  %m = and i32 %i, 3
  %x = zext i32 %m to i64
  %pa = getelementptr inbounds [4 x i32], ptr %a, i64 0, i64 %x
  %pc1 = getelementptr inbounds [4 x i32], ptr %c, i64 0, i64 1
  %pa1 = getelementptr inbounds [4 x i32], ptr %a, i64 0, i64 1
  %me = and i32 %i, 2
  %xe = zext i32 %me to i64
  %pe = getelementptr inbounds [4 x i32], ptr %a, i64 0, i64 %xe
)IR" + body + R"IR(  %dec = add i32 %i, -1
  br label %head
done:
  %mj = and i32 %j, 3
  %y = zext i32 %mj to i64
  %ra = getelementptr inbounds [4 x i32], ptr %a, i64 0, i64 %y
  %rc1 = getelementptr inbounds [4 x i32], ptr %c, i64 0, i64 1
)IR" + after + R"IR(  ret i32 %r
}
)IR";
}

const char* kZeroBoth =
    "  call void @llvm.memset.p0.i64(ptr align 16 %a, i8 0, i64 16, i1 false)\n"
    "  call void @llvm.memset.p0.i64(ptr align 16 %c, i8 1, i64 16, i1 false)\n";
const char* kZeroC = "  call void @llvm.memset.p0.i64(ptr align 16 %c, i8 1, i64 16, i1 false)\n";
const char* kReadA = "  %r = load i32, ptr %ra, align 4\n";
// p = (i == 20) ? 0 : p (a write that happens once and is never undone)
std::string kFirstZero(const std::string& p) {
    return "  %old = load i32, ptr " + p + ", align 4\n  %first0 = icmp eq i32 %i, 20\n"
           "  %val = select i1 %first0, i32 0, i32 %old\n  store i32 %val, ptr " + p + ", align 4\n";
}
const char* kDivC = "  %v = load i32, ptr %rc1, align 4\n  %r = udiv i32 100, %v\n";

struct KCase {
    std::string name, ir;
    const char* status;
    const char* kind;       // extra.k_induction
    const char* footprint;  // substring of extra.k_induction_footprint ("" = none)
    std::vector<uint64_t> witness;  // false cases: the interpreter finds the violation
};

std::vector<KCase> cases() {
    return {
        // a[n & 3] = 1 over a zeroed array: the read after the loop is initialised
        {"km_fill", loop_fn("km_fill", kZeroBoth, "  store i32 1, ptr %pa, align 4\n", kReadA), "PROVED-UNBOUNDED",
         "closed", "1 object(s): 1 havocked; initialised flags old or arbitrary", {}},
        // a[n & 2] = 1 over an uninitialised array: the odd elements are never
        // written (the havoc must not assume the written bytes initialised)
        {"km_uninit", loop_fn("km_uninit", kZeroC, "  store i32 1, ptr %pe, align 4\n", kReadA), "BOUNDED",
         "step-open", "old or arbitrary", {20, 1, 0}},
        // the loop writes a[1]; the divisor c[1] is outside the footprint
        {"km_other", loop_fn("km_other", kZeroBoth, "  store i32 0, ptr %pa1, align 4\n", kDivC), "PROVED-UNBOUNDED",
         "closed", "1 object(s)", {}},
        // the loop writes c[1] = 0 in its first iteration (n == 20) only,
        // through a pointer computed in the loop: without the havoc the
        // step would see c[1] == 1 and close
        {"km_alias", loop_fn("km_alias", kZeroBoth, kFirstZero("%pc1"), kDivC), "BOUNDED", "step-open",
         "1 object(s)", {20, 0, 0}},
        // the target is a select between the two arrays: every object is havocked
        {"km_select", loop_fn("km_select", kZeroBoth,
                              "  %q = select i1 %s, ptr %pa1, ptr %pc1\n" + kFirstZero("%q"), kDivC),
         "BOUNDED", "step-open", "every object allocated before the loop", {20, 0, 0}},
        {"km_select_ok", loop_fn("km_select_ok", kZeroBoth,
                                 "  %q = select i1 %s, ptr %pa1, ptr %pc1\n" + kFirstZero("%q"),
                                 "  %v0 = load i32, ptr %rc1, align 4\n  %v = or i32 %v0, 1\n  %r = udiv i32 100, %v\n"),
         "PROVED-UNBOUNDED", "closed", "every object allocated before the loop", {}},
        // memcpy from an uninitialised array copies its initialised flags:
        // the havoc makes them arbitrary, not "old or arbitrary"
        // (the copy happens in the first iteration only: len = n == 20 ? 16 : 0)
        {"km_memcpy", loop_fn("km_memcpy", kZeroC,
                              "  %first = icmp eq i32 %i, 20\n  %len = select i1 %first, i64 16, i64 0\n"
                              "  call void @llvm.memcpy.p0.p0.i64(ptr align 16 %c, ptr align 16 %a, i64 %len, i1 false)\n",
                              "  %r = load i32, ptr %rc1, align 4\n"),
         "BOUNDED", "step-open", "initialised flags arbitrary", {20, 0, 0}},
    };
}

}  // namespace kind_mem

TEST_CASE("pir mem: k-induction havocs the write footprint of a memory-writing loop") {
    for (auto& c : kind_mem::cases()) {
        CAPTURE(c.name);
        auto t = mem_tr(with_decls(c.ir), c.name);
        REQUIRE_MESSAGE(t.fn.has_value(), t.reason);
        for (auto enc : {pp::MemEncoding::Array, pp::MemEncoding::Bv}) {
            CAPTURE(static_cast<int>(enc));
            pp::EncodeOptions eo;
            eo.memory = enc;
            auto v = pp::check_function(*t.fn, 8, 30, eo);
            CAPTURE(v.message);
            CHECK(v.status == c.status);
            CHECK(v.extra["k_induction"] == c.kind);
            CHECK(v.extra["k_induction_memory"] == "write footprint havocked");
            CHECK(v.extra["k_induction_footprint"].find(c.footprint) != std::string::npos);
        }
        if (!c.witness.empty()) {
            // the false cases are real: the interpreter hits the violation
            auto r = pp::interpret(*t.fn, c.witness);
            CHECK(r.status == pp::InterpResult::Violation);
        }
    }
}

TEST_CASE("pir mem: k-induction is not attempted for a loop that allocates or frees") {
    const char* ir = R"IR(define i32 @km_free(i32 %n) {
entry:
  %p = call ptr @__prism_alloc(i64 4, i32 2, i32 1)
  br label %head
head:
  %i = phi i32 [ %n, %entry ], [ %dec, %latch ]
  %go = icmp ne i32 %i, 0
  br i1 %go, label %body, label %done
body:
  %last = icmp eq i32 %i, 1
  br i1 %last, label %rel, label %latch
rel:
  call void @__prism_free(ptr %p, i32 2)
  br label %latch
latch:
  %dec = add i32 %i, -1
  br label %head
done:
  ret i32 0
}
)IR";
    auto t = mem_tr(with_decls(ir), "km_free");
    REQUIRE_MESSAGE(t.fn.has_value(), t.reason);
    auto v = pp::check_function(*t.fn, 8, 30);
    CHECK(v.status == prism::laws::BOUNDED);
    CHECK(v.extra["k_induction"] == "not-attempted (allocation or free in the loop)");
}
#endif
