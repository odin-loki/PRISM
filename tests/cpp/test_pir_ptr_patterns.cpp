// Doctests: PTRPARAM pattern answers (pir-ptr-patterns): --pir-declared-types (a harness pointee takes its declared
// type and size from the debug information), --pir-param-preconditions (per-parameter preconditions, named), and a
// --pir-field-invariants "valid" row on a pointer field that already has its own harness object. Each test also
// checks that a real violation one byte / one element past what the answer allows is still FAILED.
// Linked into prism_tests next to test_main.cpp (which provides main()).
#include <doctest/doctest.h>
#ifdef ERROR
#  undef ERROR
#endif

#include "prism/laws.hpp"
#include "prism/pir.hpp"

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

namespace pp = prism::pir;

struct Run {
    std::string status;
    std::string msg;
    std::vector<std::string> assumptions;
};

bool has(const std::vector<std::string>& v, const std::string& s) {
    for (auto& x : v)
        if (x.find(s) != std::string::npos) return true;
    return false;
}

Run run(const std::string& ir, const std::string& fn, const pp::TranslateOptions& base) {
    static std::vector<pp::ir::Module> keep;  // translations point into their module
    keep.push_back(pp::ir::parse_module_kernel(ir));
    const auto* f = keep.back().find(fn);
    REQUIRE(f != nullptr);
    pp::TranslateOptions o = base;
    o.kernel_api = "linux";
    o.auto_harness.on = true;
    auto t = pp::translate(keep.back(), *f, o);
    if (!t.fn) return {std::string(prism::laws::NEEDS_HARNESS), t.reason, {}};
    auto v = pp::check_function(*t.fn, 4, 30.0);
    return {v.status, v.message, t.fn->assumptions};
}

// A byte read at p[k], guarded by k < LIMIT (or k <= LIMIT: one past). p is declared `struct mount *` (352 bytes) in
// the debug information; the IR only reads bytes through it, so without the declaration the harness guesses 16.
std::string declared_ir(const char* pred, int limit, bool ir_struct, bool typedef_) {
    std::string s;
    if (ir_struct) s += "%struct.mount = type { [352 x i8] }\n";
    s += "define i8 @rd(ptr %p, i64 %k) !dbg !10 {\nentry:\n  %c = icmp " + std::string(pred) + " i64 %k, " +
         std::to_string(limit) +
         "\n  br i1 %c, label %ok, label %out\nok:\n  %e = getelementptr inbounds i8, ptr %p, i64 %k\n"
         "  %x = load i8, ptr %e\n  ret i8 %x\nout:\n  ret i8 0\n}\n";
    // h->mnt (offset 8 of struct holder) is a `struct mount *`: the nested object of the loaded field
    s += "define i8 @rdh(ptr %h, i64 %k) !dbg !30 {\nentry:\n  %f = getelementptr inbounds i8, ptr %h, i64 8\n"
         "  %m = load ptr, ptr %f\n  %c = icmp " + std::string(pred) + " i64 %k, " + std::to_string(limit) +
         "\n  br i1 %c, label %ok, label %out\nok:\n  %e = getelementptr inbounds i8, ptr %m, i64 %k\n"
         "  %x = load i8, ptr %e\n  ret i8 %x\nout:\n  ret i8 0\n}\n";
    s += "!1 = !DIFile(filename: \"t.c\", directory: \"/tmp\")\n"
         "!10 = distinct !DISubprogram(name: \"rd\", scope: !1, file: !1, line: 1, unit: !0)\n"
         "!20 = !DILocalVariable(name: \"p\", arg: 1, scope: !10, file: !1, line: 1, type: !21)\n";
    s += typedef_ ? "!21 = !DIDerivedType(tag: DW_TAG_pointer_type, baseType: !24, size: 64)\n"
                    "!24 = !DIDerivedType(tag: DW_TAG_typedef, name: \"mount_t\", file: !1, line: 2, baseType: !25)\n"
                    "!25 = !DIDerivedType(tag: DW_TAG_const_type, baseType: !22)\n"
                  : "!21 = !DIDerivedType(tag: DW_TAG_pointer_type, baseType: !22, size: 64)\n";
    s += "!22 = distinct !DICompositeType(tag: DW_TAG_structure_type, name: \"mount\", file: !1, line: 3, size: 2816, elements: !23)\n"
         "!23 = !{}\n"
         "!30 = distinct !DISubprogram(name: \"rdh\", scope: !1, file: !1, line: 9, unit: !0)\n"
         "!31 = !DILocalVariable(name: \"h\", arg: 1, scope: !30, file: !1, line: 9, type: !32)\n"
         "!32 = !DIDerivedType(tag: DW_TAG_pointer_type, baseType: !33, size: 64)\n"
         "!33 = distinct !DICompositeType(tag: DW_TAG_structure_type, name: \"holder\", file: !1, line: 5, size: 128, elements: !34)\n"
         "!34 = !{!35, !36}\n"
         "!35 = !DIDerivedType(tag: DW_TAG_member, name: \"flags\", scope: !33, file: !1, line: 6, baseType: !37, size: 64)\n"
         "!36 = !DIDerivedType(tag: DW_TAG_member, name: \"mnt\", scope: !33, file: !1, line: 7, baseType: !21, size: 64, offset: 64)\n"
         "!37 = !DIBasicType(name: \"unsigned long\", size: 64, encoding: DW_ATE_unsigned)\n";
    return s;
}

pp::TranslateOptions declared(bool on) {
    pp::TranslateOptions o;
    o.auto_harness.declared_types = on;
    return o;
}

}  // namespace

TEST_CASE("pir ptr patterns: the parser keeps debug type nodes, parameter variables and ref tuples") {
    auto m = pp::ir::parse_module_kernel(declared_ir("ult", 352, false, true));
    REQUIRE(m.ditypes.count("!22"));
    CHECK(m.ditypes["!22"].tag == "DW_TAG_structure_type");
    CHECK(m.ditypes["!22"].size_bits == 2816);
    CHECK(m.ditypes["!36"].offset_bits == 64);
    CHECK(m.ditypes["!24"].base == "!25");
    REQUIRE(m.md_tuples.count("!34"));
    CHECK(m.md_tuples["!34"].size() == 2);
    REQUIRE(m.diparams.size() == 2);
    CHECK(m.diparams[0].name == "p");
    CHECK(m.diparams[0].scope == "!10");
}

#ifdef PRISM_HAS_Z3
TEST_CASE("pir ptr patterns: --pir-declared-types sizes a pointee from its declaration, named; one past it still FAILED") {
    for (bool irs : {false, true})
        for (bool td : {false, true}) {
            CAPTURE(irs);
            CAPTURE(td);
            const auto ir = declared_ir("ult", 352, irs, td);
            // the default: 16 bytes guessed for a byte-indexed pointee, so p[20] is out of bounds
            CHECK(run(ir, "rd", declared(false)).status == prism::laws::FAILED);
            auto r = run(ir, "rd", declared(true));
            CHECK_MESSAGE(r.status == prism::laws::PROVED, (r.status + " " + r.msg));
            CHECK(has(r.assumptions, "declared pointee type struct mount of 352 bytes (--pir-declared-types"));
            CHECK(has(r.assumptions, "declared-type: p points to struct mount (352 bytes, from the declaration"));
            CHECK_FALSE(has(r.assumptions, "harness-size-from-access:"));
            CHECK(has(r.assumptions, "points to the start of a valid object of 352 bytes"));
            // k <= 352 reads byte 352 of a 352-byte struct: a real violation, still caught
            CHECK(run(declared_ir("ule", 352, irs, td), "rd", declared(true)).status == prism::laws::FAILED);
        }
}

TEST_CASE("pir ptr patterns: --pir-declared-types sizes a nested pointer field from its member's declaration") {
    pp::TranslateOptions o = declared(true);
    o.auto_harness.depth = 2;
    pp::TranslateOptions off = o;
    off.auto_harness.declared_types = false;
    CHECK(run(declared_ir("ult", 352, false, false), "rdh", off).status == prism::laws::FAILED);
    auto r = run(declared_ir("ult", 352, false, false), "rdh", o);
    CHECK_MESSAGE(r.status == prism::laws::PROVED, (r.status + " " + r.msg));
    CHECK(has(r.assumptions, "h->@8 points to the start of a valid object of 352 bytes"));
    CHECK(run(declared_ir("ule", 352, false, false), "rdh", o).status == prism::laws::FAILED);
}
#endif

namespace {

const char* kParams = R"IR(%struct.big = type { [100 x i8] }
%struct.dev = type { ptr, i32, i32 }
define i32 @div(i32 %x) {
entry:
  %q = sdiv i32 100, %x
  ret i32 %q
}
define i8 @rd(ptr %p, i64 %k) {
entry:
  %c = icmp ult i64 %k, 100
  br i1 %c, label %ok, label %out
ok:
  %e = getelementptr inbounds i8, ptr %p, i64 %k
  %x = load i8, ptr %e
  ret i8 %x
out:
  ret i8 0
}
define i8 @rdf(ptr %d, i64 %k) {
entry:
  %f = getelementptr inbounds %struct.dev, ptr %d, i32 0, i32 0
  %b = load ptr, ptr %f
  %c = icmp ult i64 %k, 32
  br i1 %c, label %ok, label %out
ok:
  %e = getelementptr inbounds i8, ptr %b, i64 %k
  %x = load i8, ptr %e
  ret i8 %x
out:
  ret i8 0
}
)IR";

pp::ParamPrecondition pre(const char* fn, const char* param, const char* cond) {
    // through the loader, so the test also covers the file format
    static int n = 0;
    const auto path = std::filesystem::temp_directory_path() / ("prism_pp_" + std::to_string(++n) + ".tsv");
    {
        std::ofstream o(path);
        o << "function\tparameter\tcondition\tscope\tstatus\tsource\n# a comment\n"
          << fn << "\t" << param << "\t" << cond << "\t*\tUNVALIDATED\ttest\n";
    }
    std::string err;
    auto rows = pp::load_param_preconditions(path, err);
    std::filesystem::remove(path);
    REQUIRE_MESSAGE(err.empty(), err);
    REQUIRE(rows.size() == 1);
    return rows[0];
}

pp::TranslateOptions with(std::vector<pp::ParamPrecondition> rows) {
    pp::TranslateOptions o;
    o.param_preconditions = std::move(rows);
    return o;
}

}  // namespace

TEST_CASE("pir ptr patterns: --pir-param-preconditions file format") {
    const auto path = std::filesystem::temp_directory_path() / "prism_pp_bad.tsv";
    auto load = [&](const std::string& line) {
        {
            std::ofstream o(path);
            o << line << "\n";
        }
        std::string err;
        auto rows = pp::load_param_preconditions(path, err);
        std::filesystem::remove(path);
        return std::make_pair(rows, err);
    };
    CHECK(load("f\tp\tvalid 352\t*\tUNVALIDATED\thaiku b01").second.empty());
    CHECK(load("f\t0\trange -3 7\tfs/namespace.c\tREVIEWED\tsmatch").second.empty());
    auto t = load("@f\tm\ttype struct mount\t*\tUNVALIDATED\tx");
    REQUIRE(t.first.size() == 1);
    CHECK(t.first[0].function == "f");
    CHECK(t.first[0].type_name == "%struct.mount");
    CHECK(load("f\tm\ttype %union.u\t*\tUNVALIDATED\tx").first.at(0).type_name == "%union.u");
    CHECK(load("f\tm\tnonnull\t\tUNVALIDATED\tx").first.at(0).scope == "*");
    CHECK_FALSE(load("f\tp\tvalid 0\t*\tUNVALIDATED\tx").second.empty());
    CHECK_FALSE(load("f\tp\trange 5 1\t*\tUNVALIDATED\tx").second.empty());
    CHECK_FALSE(load("f\tp\tbigger 5\t*\tUNVALIDATED\tx").second.empty());
    CHECK_FALSE(load("f\tp\tvalid 8\t*\t\tx").second.empty());          // status required
    CHECK_FALSE(load("f\tp\tvalid 8\t*\tUNVALIDATED").second.empty());  // 5 columns
}

#ifdef PRISM_HAS_Z3
TEST_CASE("pir ptr patterns: --pir-param-preconditions range / valid / type rows, named; wrong rows prove nothing more") {
    CHECK(run(kParams, "div", {}).status == prism::laws::FAILED);
    auto r = run(kParams, "div", with({pre("div", "x", "range 1 10")}));
    CHECK_MESSAGE(r.status == prism::laws::PROVED, (r.status + " " + r.msg));
    CHECK(has(r.assumptions, "param-precondition: div(x) range 1 10 (--pir-param-preconditions; scope *; status UNVALIDATED"));
    CHECK(run(kParams, "div", with({pre("div", "0", "range 1 10")})).status == prism::laws::PROVED);  // by index
    CHECK(run(kParams, "div", with({pre("div", "x", "range 0 10")})).status == prism::laws::FAILED);  // 0 still in range
    CHECK(run(kParams, "div", with({pre("other", "x", "range 1 10")})).status == prism::laws::FAILED);
    CHECK(run(kParams, "div", with({pre("div", "y", "range 1 10")})).status == prism::laws::FAILED);
    // a bound that is not a signed 32-bit value: the row does not apply
    CHECK(run(kParams, "div", with({pre("div", "x", "range 1 4294967296")})).status == prism::laws::FAILED);

    CHECK(run(kParams, "rd", {}).status == prism::laws::FAILED);  // 16 bytes guessed
    auto v = run(kParams, "rd", with({pre("rd", "p", "valid 100")}));
    CHECK_MESSAGE(v.status == prism::laws::PROVED, (v.status + " " + v.msg));
    CHECK(has(v.assumptions, "param-precondition: rd(p) valid 100"));
    CHECK(has(v.assumptions, "at least 100 bytes stated by a parameter precondition"));
    // one byte short: p[99] is past a 99-byte object, a real violation, still caught
    CHECK(run(kParams, "rd", with({pre("rd", "p", "valid 99")})).status == prism::laws::FAILED);
    auto ty = run(kParams, "rd", with({pre("rd", "p", "type struct big")}));
    CHECK_MESSAGE(ty.status == prism::laws::PROVED, (ty.status + " " + ty.msg));
    CHECK(has(ty.assumptions, "param-precondition: rd(p) type struct big"));
    // a type this unit does not have: not applied
    CHECK(run(kParams, "rd", with({pre("rd", "p", "type struct nosuch")})).status == prism::laws::FAILED);
    // nonnull adds nothing to the harness (its object is already non-null): not applied, not named
    auto nn = run(kParams, "rd", with({pre("rd", "p", "nonnull")}));
    CHECK(nn.status == prism::laws::FAILED);
    CHECK_FALSE(has(nn.assumptions, "param-precondition:"));
}

TEST_CASE("pir ptr patterns: a field-invariant valid row grows a nested harness object; one short still FAILED") {
    auto fi = [](int64_t n) {
        pp::FieldInvariant x;
        x.struct_type = "%struct.dev";
        x.offset = 0;
        x.width = 8;
        x.kind = "valid";
        x.a = n;
        x.status = "UNVALIDATED";
        x.source = "test";
        return x;
    };
    pp::TranslateOptions o;
    o.auto_harness.depth = 2;  // d->@0 gets its own harness object (16 bytes guessed)
    CHECK(run(kParams, "rdf", o).status == prism::laws::FAILED);
    o.field_invariants = {fi(32)};
    auto r = run(kParams, "rdf", o);
    CHECK_MESSAGE(r.status == prism::laws::PROVED, (r.status + " " + r.msg));
    CHECK(has(r.assumptions, "points to a valid object of 32 bytes (its harness object grown from 16 bytes)"));
    o.field_invariants = {fi(31)};
    CHECK(run(kParams, "rdf", o).status == prism::laws::FAILED);
}
#endif

namespace {

// q is declared `int (*q)[4]` (a 16-byte array); q4 reads bytes 20..23 of it (q[0][5]), q4ok bytes 12..15.
// qk has no debug information: q[1000] of an i32 pointee.
const char* kAccessSized = R"IR(define i32 @q4(ptr %q) !dbg !50 {
entry:
  %e = getelementptr inbounds i8, ptr %q, i64 20
  %x = load i32, ptr %e
  ret i32 %x
}
define i32 @q4ok(ptr %q) !dbg !60 {
entry:
  %e = getelementptr inbounds i8, ptr %q, i64 12
  %x = load i32, ptr %e
  ret i32 %x
}
define i32 @qk(ptr %q) {
entry:
  %e = getelementptr inbounds i32, ptr %q, i64 1000
  %x = load i32, ptr %e
  ret i32 %x
}
!1 = !DIFile(filename: "t.c", directory: "/tmp")
!50 = distinct !DISubprogram(name: "q4", scope: !1, file: !1, line: 1, unit: !0)
!51 = !DILocalVariable(name: "q", arg: 1, scope: !50, file: !1, line: 1, type: !52)
!52 = !DIDerivedType(tag: DW_TAG_pointer_type, baseType: !53, size: 64)
!53 = !DICompositeType(tag: DW_TAG_array_type, baseType: !56, size: 128, elements: !54)
!54 = !{!55}
!55 = !DISubrange(count: 4)
!56 = !DIBasicType(name: "int", size: 32, encoding: DW_ATE_signed)
!60 = distinct !DISubprogram(name: "q4ok", scope: !1, file: !1, line: 5, unit: !0)
!61 = !DILocalVariable(name: "q", arg: 1, scope: !60, file: !1, line: 5, type: !52)
)IR";

}  // namespace

#ifdef PRISM_HAS_Z3
TEST_CASE("pir ptr patterns: a declared size is the object's size: a constant access past it is FAILED, not grown") {
    // without declared types the object grows to the access (q[0][5] is "proved"), tagged harness-size-from-access
    auto g = run(kAccessSized, "q4", declared(false));
    CHECK_MESSAGE(g.status == prism::laws::PROVED, (g.status + " " + g.msg));
    CHECK(has(g.assumptions, "harness-size-from-access: q sized to reach the highest constant offset accessed (24 bytes"));
    // a declared int[4] read at element 5: a real violation, caught
    auto d = run(kAccessSized, "q4", declared(true));
    CHECK(d.status == prism::laws::FAILED);
    auto ok = run(kAccessSized, "q4ok", declared(true));
    CHECK_MESSAGE(ok.status == prism::laws::PROVED, (ok.status + " " + ok.msg));
    CHECK(has(ok.assumptions, "declared-type: q points to DW_TAG_array_type (16 bytes, from the declaration"));
    CHECK_FALSE(has(ok.assumptions, "harness-size-from-access:"));
}

TEST_CASE("pir ptr patterns: an untyped q[1000] is sized from the access, and tagged so") {
    for (bool on : {false, true}) {
        auto r = run(kAccessSized, "qk", declared(on));
        CHECK_MESSAGE(r.status == prism::laws::PROVED, (r.status + " " + r.msg));
        CHECK(has(r.assumptions, "harness-size-from-access: q sized to reach the highest constant offset accessed (4004 bytes"));
        CHECK_FALSE(has(r.assumptions, "declared-type:"));
    }
}
#endif

namespace {

// netdev_priv(dev): the private area starts at the end of the 64-byte struct net_device
std::string priv_ir(bool alloc) {
    std::string s = R"IR(%struct.net_device = type { [64 x i8] }
%struct.mypriv = type { [8 x i32] }
define internal ptr @netdev_priv(ptr %dev) {
entry:
  %p = getelementptr inbounds i8, ptr %dev, i64 64
  ret ptr %p
}
define i32 @rd_priv(ptr %dev, i64 %k) {
entry:
  %f = getelementptr inbounds %struct.net_device, ptr %dev, i32 0, i32 0
  %x0 = load i8, ptr %f
  %p = call ptr @netdev_priv(ptr %dev)
  %c = icmp ult i64 %k, 8
  br i1 %c, label %ok, label %out
ok:
  %a = getelementptr inbounds %struct.mypriv, ptr %p, i32 0, i32 0, i64 %k
  %x = load i32, ptr %a
  ret i32 %x
out:
  ret i32 0
}
define i32 @rd_priv_at(ptr %dev) {
entry:
  %f = getelementptr inbounds %struct.net_device, ptr %dev, i32 0, i32 0
  %x0 = load i8, ptr %f
  %p = call ptr @netdev_priv(ptr %dev)
  %a = getelementptr inbounds %struct.mypriv, ptr %p, i32 0, i32 0, i64 0
  %b = getelementptr inbounds i8, ptr %a, i64 OFF
  %x = load i32, ptr %b
  ret i32 %x
}
)IR";
    if (alloc)
        s += "declare ptr @alloc_netdev_mqs(i32, ptr, i8, ptr, i32, i32)\n"
             "define ptr @probe() {\nentry:\n  %d = call ptr @alloc_netdev_mqs(i32 48, ptr null, i8 0, ptr null, i32 1, i32 1)\n"
             "  ret ptr %d\n}\n";
    return s;
}

std::string at(std::string s, int off) {
    s.replace(s.find("i64 OFF"), 7, "i64 " + std::to_string(off));
    return s;
}

pp::TranslateOptions priv(bool on) {
    pp::TranslateOptions o;
    o.container_of = true;  // netdev_priv(dev) is followed as dev + 64 (--pir-container-of)
    o.auto_harness.priv_areas = on;
    return o;
}

}  // namespace

#ifdef PRISM_HAS_Z3
TEST_CASE("pir ptr patterns: --pir-priv-areas sizes a private area past the struct end; one past it still FAILED") {
    CHECK(run(priv_ir(false), "rd_priv", priv(false)).status == prism::laws::FAILED);
    auto r = run(priv_ir(false), "rd_priv", priv(true));
    CHECK_MESSAGE(r.status == prism::laws::PROVED, (r.status + " " + r.msg));
    CHECK(has(r.assumptions, "priv-area: dev has a private area at byte 64 of 32 bytes (%struct.mypriv is based there"));
    CHECK(has(r.assumptions, "of the private area's struct (no allocation with a constant private size in this unit)"));
    // bytes 28..31 of the area: inside; bytes 32..35: one past the 32-byte area, a real violation
    CHECK(run(at(priv_ir(false), 28), "rd_priv_at", priv(true)).status == prism::laws::PROVED);
    auto past = run(at(priv_ir(false), 32), "rd_priv_at", priv(true));
    CHECK(past.status == prism::laws::FAILED);
    CHECK_FALSE(has(past.assumptions, "harness-size-from-access:"));
    // the unit allocates the device with a 48-byte private area: sized from the allocation
    auto al = run(at(priv_ir(true), 44), "rd_priv_at", priv(true));
    CHECK_MESSAGE(al.status == prism::laws::PROVED, (al.status + " " + al.msg));
    CHECK(has(al.assumptions, "of 48 bytes"));
    CHECK(has(al.assumptions, "from alloc_netdev_mqs(sizeof_priv = 48) in this unit"));
    CHECK(run(at(priv_ir(true), 48), "rd_priv_at", priv(true)).status == prism::laws::FAILED);
}
#endif

namespace {

// t->q[t->tail] with q an in-object [4 x i32]
const char* kIndexField = R"IR(%struct.tw = type { [4 x i32], i32 }
define void @put(ptr %t, i32 %v) {
entry:
  %tp = getelementptr inbounds %struct.tw, ptr %t, i32 0, i32 1
  %i = load i32, ptr %tp
  %ix = sext i32 %i to i64
  %a = getelementptr inbounds %struct.tw, ptr %t, i32 0, i32 0, i64 %ix
  store i32 %v, ptr %a
  ret void
}
define void @put_two(ptr %t, i32 %v) {
entry:
  %tp = getelementptr inbounds %struct.tw, ptr %t, i32 0, i32 1
  %i = load i32, ptr %tp
  %ix = sext i32 %i to i64
  %a = getelementptr inbounds %struct.tw, ptr %t, i32 0, i32 0, i64 %ix
  store i32 %v, ptr %a
  %i1 = add nsw i32 %i, 1
  %ix1 = sext i32 %i1 to i64
  %b = getelementptr inbounds %struct.tw, ptr %t, i32 0, i32 0, i64 %ix1
  store i32 %v, ptr %b
  ret void
}
define void @put_after_set(ptr %t, i32 %v) {
entry:
  %tp = getelementptr inbounds %struct.tw, ptr %t, i32 0, i32 1
  store i32 4, ptr %tp
  %i = load i32, ptr %tp
  %ix = sext i32 %i to i64
  %q = getelementptr inbounds %struct.tw, ptr %t, i32 0, i32 0
  %a = getelementptr inbounds [4 x i32], ptr %q, i64 0, i64 %ix
  store i32 %v, ptr %a
  ret void
}
)IR";

pp::TranslateOptions ifb(bool on) {
    pp::TranslateOptions o;
    o.auto_harness.index_field_bounds = on;
    return o;
}

}  // namespace

#ifdef PRISM_HAS_Z3
TEST_CASE("pir ptr patterns: --pir-index-field-bounds: an array-index field is in range, named; one past the bound still FAILED") {
    CHECK(run(kIndexField, "put", ifb(false)).status == prism::laws::FAILED);
    auto r = run(kIndexField, "put", ifb(true));
    CHECK_MESSAGE(r.status == prism::laws::PROVED, (r.status + " " + r.msg));
    CHECK(has(r.assumptions, "index-field-bound: t->@16 (4 bytes) is in [0, 3]: it indexes the %struct.tw array of 4 at byte 0"));
    // q[tail + 1]: tail = 3 is allowed by the bound, and index 4 is one past the array: caught
    CHECK(run(kIndexField, "put_two", ifb(true)).status == prism::laws::FAILED);
    // the function itself sets tail = 4 before indexing (the [4 x i32] form): the store wins, caught
    CHECK(run(kIndexField, "put_after_set", ifb(true)).status == prism::laws::FAILED);
}
#endif

namespace {

// tcp_sk(sk): sk is declared `struct sock *` (16 bytes) and used as the larger struct tcp_sock that embeds it first
const char* kSubtype = R"IR(%struct.sock = type { [16 x i8] }
%struct.tcp_sock = type { %struct.sock, i64, i64 }
define i64 @rd_tcp(ptr %sk) !dbg !50 {
entry:
  %f0 = getelementptr inbounds %struct.sock, ptr %sk, i32 0, i32 0
  %x0 = load i8, ptr %f0
  %f = getelementptr inbounds %struct.tcp_sock, ptr %sk, i32 0, i32 2
  %x = load i64, ptr %f
  ret i64 %x
}
!1 = !DIFile(filename: "t.c", directory: "/tmp")
!50 = distinct !DISubprogram(name: "rd_tcp", scope: !1, file: !1, line: 1, unit: !0)
!51 = !DILocalVariable(name: "sk", arg: 1, scope: !50, file: !1, line: 1, type: !52)
!52 = !DIDerivedType(tag: DW_TAG_pointer_type, baseType: !53, size: 64)
!53 = distinct !DICompositeType(tag: DW_TAG_structure_type, name: "sock", file: !1, line: 3, size: 128, elements: !54)
!54 = !{}
)IR";

}  // namespace

#ifdef PRISM_HAS_Z3
TEST_CASE("pir ptr patterns: a declaration smaller than a struct the code uses at offset 0 is not used (tcp_sk)") {
    auto r = run(kSubtype, "rd_tcp", declared(true));
    CHECK_MESSAGE(r.status == prism::laws::PROVED, (r.status + " " + r.msg));
    CHECK_FALSE(has(r.assumptions, "declared-type:"));
    CHECK(has(r.assumptions, "points to the start of a valid object of 32 bytes"));
    CHECK(has(r.assumptions, "container-type-from-cast: sk is treated as %struct.tcp_sock (32 bytes; a getelementptr uses it at offset 0 in rd_tcp"));
}
#endif

namespace {

// h->buf is only passed to the heap free (no type evidence): without --pir-heap-freed it stays arbitrary
const char* kFreed = R"IR(%struct.h = type { ptr, ptr }
declare void @__prism_free(ptr, i32)
define void @rel(ptr %h) {
entry:
  %f = getelementptr inbounds %struct.h, ptr %h, i32 0, i32 0
  %b = load ptr, ptr %f
  call void @__prism_free(ptr %b, i32 2)
  ret void
}
define void @rel_twice(ptr %h) {
entry:
  %f = getelementptr inbounds %struct.h, ptr %h, i32 0, i32 0
  %b = load ptr, ptr %f
  call void @__prism_free(ptr %b, i32 2)
  call void @__prism_free(ptr %b, i32 2)
  ret void
}
define void @rel_inner(ptr %h) {
entry:
  %f = getelementptr inbounds %struct.h, ptr %h, i32 0, i32 1
  call void @__prism_free(ptr %f, i32 2)
  ret void
}
)IR";

pp::TranslateOptions freed(bool on) {
    pp::TranslateOptions o;
    o.auto_harness.heap_freed = on;
    return o;
}

}  // namespace

#ifdef PRISM_HAS_Z3
TEST_CASE("pir ptr patterns: --pir-heap-freed: a freed harness field is a heap object, named; double free and non-heap free still FAILED") {
    CHECK(run(kFreed, "rel", freed(false)).status == prism::laws::FAILED);
    auto r = run(kFreed, "rel", freed(true));
    CHECK_MESSAGE(r.status == prism::laws::PROVED, (r.status + " " + r.msg));
    CHECK(has(r.assumptions, "heap-object: h->@0 is heap-allocated (freed by __prism_free in rel)"));
    // freed twice: the second free of the heap object is a double free
    auto d = run(kFreed, "rel_twice", freed(true));
    CHECK(d.status == prism::laws::FAILED);
    // a pointer into the (non-heap) parameter object is freed: still an invalid free
    CHECK(run(kFreed, "rel_inner", freed(true)).status == prism::laws::FAILED);
}
#endif
