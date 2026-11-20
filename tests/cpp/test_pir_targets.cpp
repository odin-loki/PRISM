// A unit is checked for its own target (docs/PIR.md "Targets"): its
// --compile-commands --target (and the ABI flags of a non-x86-64 target) are
// kept, and PIR takes pointer width, byte order and layout from the IR's
// datalayout and triple. The same C source is checked as x86-64, i386, arm
// (ILP32 little-endian), mips, m68k (ILP32 big-endian, m68k with 2-byte int
// alignment), s390x and powerpc64 (LP64 big-endian); the verdicts differ
// exactly where the target's semantics differ.
#include <doctest/doctest.h>
#ifdef ERROR
#  undef ERROR
#endif

#include "prism/config.hpp"
#include "prism/laws.hpp"
#include "prism/pir.hpp"
#include "prism/pir/frontend.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#ifndef _WIN32

namespace {

namespace fs = std::filesystem;
namespace pp = prism::pir;
namespace L = prism::laws;

bool skip_without_frontend() {
    auto cfg = prism::default_config();
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

const fs::path kDir = fs::temp_directory_path() / ("prism_pir_targets_" + std::to_string(::getpid()));

// No system header: a cross libc need not be installed. Every function is
// pointer-free at its interface, so each is checked without a harness.
const char* kSrc = R"(typedef __SIZE_TYPE__ size_t;
void *memcpy(void *, const void *, size_t);
void *memset(void *, int, size_t);
/* ILP32: 32-bit long and pointers */
int ptr_is_32(void) { return 100 / ((int)sizeof(void *) - 4); }
long mul_big(int a) { if (a < 0 || a > 100) return 0; return (long)a * 100000000L; }
long mul_small(int a) { if (a < 0 || a > 20) return 0; return (long)a * 100000000L; }
/* byte order: the first byte of an int in memory */
int low_first(void) { unsigned v = 0x01020304u; unsigned char *p = (unsigned char *)&v; return 100 / (p[0] - 4); }
int high_first(void) { unsigned v = 0x01020304u; unsigned char *p = (unsigned char *)&v; return 100 / (p[0] - 1); }
int short_b1(void) { unsigned short s = 0x0102; unsigned char *p = (unsigned char *)&s; return 10 / (p[1] - 2); }
static const unsigned g_word = 0x0A0B0C0Du;
int glob_b0(void) { const unsigned char *p = (const unsigned char *)&g_word; return 100 / (p[0] - 0x0D); }
/* bytes assembled into a wider value */
int assemble(void) { unsigned char b[4] = {1, 2, 3, 4}; unsigned v; memcpy(&v, b, 4); return 100 / (int)(v - 0x01020304u); }
/* a value copied through bytes comes back unchanged on every target */
int roundtrip(unsigned x) { unsigned char b[4]; unsigned y; memcpy(b, &x, 4); memcpy(&y, b, 4); return 100 / (y == x ? 1 : 0); }
long long roundtrip64(long long x) { long long y; unsigned char b[8]; memcpy(b, &x, 8); memcpy(&y, b, 8); return 100 / (y == x ? 1 : 0); }
/* layout: m68k aligns int to 2 (byte 2 is the int's first byte there, padding elsewhere) */
struct ci { char c; int i; };
int int_at2(void) { struct ci v; memset(&v, 0, sizeof v); v.i = -1; unsigned char *p = (unsigned char *)&v; return 100 / p[2]; }
/* layout: i386 aligns double to 4 (f64:32), so byte 11 is the double's last byte there */
struct cd { char c; double d; };
int dbl_b11(void) { struct cd v; memset(&v, 0, sizeof v); v.d = 1.0; unsigned char *p = (unsigned char *)&v; return 100 / p[11]; }
)";

struct Target {
    const char* name;
    std::vector<const char*> flags;
};

const std::vector<Target> kTargets = {
    {"x86_64", {"--target=x86_64-linux-gnu"}},
    {"i386", {"--target=i386-linux-gnu"}},
    {"arm", {"--target=arm-linux-gnueabi", "-mlittle-endian", "-mabi=aapcs-linux", "-marm", "-march=armv6k", "-msoft-float"}},
    {"mips", {"--target=mips-linux-gnu"}},
    {"mips64be", {"--target=mipsel-linux-gnu", "-mabi=64", "-EB", "-march=mips3"}},  // the Linux kfrag form: big-endian n64
    {"m68k", {"--target=m68k-linux-gnu"}},
    {"s390x", {"--target=s390x-linux-gnu"}},
    {"ppc64", {"--target=powerpc64-linux-gnu"}},
};

using ByFn = std::map<std::string, prism::Finding>;

std::map<std::string, ByFn> run_all() {
    fs::remove_all(kDir);
    fs::create_directories(kDir);
    std::string db = "[";
    std::vector<fs::path> files;
    for (auto& t : kTargets) {
        const auto f = kDir / (std::string("t_") + t.name + ".c");
        std::ofstream(f, std::ios::binary) << kSrc;
        files.push_back(f);
        db += std::string(db.size() > 1 ? "," : "") + "\n{\"directory\": \"" + kDir.generic_string() + "\", \"file\": \"" +
              f.generic_string() + "\", \"arguments\": [\"clang\"";
        for (auto* fl : t.flags) db += std::string(", \"") + fl + "\"";
        db += ", \"-c\", \"" + f.generic_string() + "\"]}";
    }
    db += "\n]\n";
    std::ofstream(kDir / "compile_commands.json", std::ios::binary) << db;
    auto cfg = prism::default_config();
    cfg.root = kDir;
    cfg.jobs = 2;
    cfg.compile_commands = kDir / "compile_commands.json";
    cfg.solver_cache = kDir / "cache";
    std::map<std::string, ByFn> by;
    for (auto& f : pp::run_pir(files, cfg)) {
        const auto stem = fs::path(f.file).stem().string();  // t_<target>
        if (f.function && stem.starts_with("t_")) by[stem.substr(2)][*f.function] = f;
    }
    return by;
}

bool proved(const prism::Finding& f) { return L::is_proof(f.status); }
bool failed(const prism::Finding& f, const char* cls) { return f.status == L::FAILED && f.cls == cls; }

std::string extra(const prism::Finding& f, const char* k) {
    auto it = f.extra.find(k);
    return it == f.extra.end() ? std::string() : it->second;
}

}  // namespace

TEST_CASE("pir targets: one source, checked for x86-64, ILP32 and big-endian targets") {
    if (skip_without_frontend()) return;
    auto by = run_all();
    for (auto& t : kTargets) {
        INFO(t.name);
        REQUIRE_MESSAGE(by.count(t.name), "no rows for this target");
        // every row of a non-x86-64 unit names the target it was checked for
        if (std::string(t.name) != "x86_64")
            for (auto& [fn, f] : by[t.name]) CHECK_MESSAGE(!extra(f, "target").empty(), fn);
        else
            for (auto& [fn, f] : by[t.name]) CHECK(extra(f, "target").empty());
    }
    auto st = [&](const char* t, const char* fn) {
        const auto& f = by[t][fn];
        return std::string(t) + " " + fn + ": " + f.status + " " + f.cls + " " + f.message;
    };
    const std::vector<std::string> ilp32 = {"i386", "arm", "mips", "m68k"}, lp64 = {"x86_64", "mips64be", "s390x", "ppc64"};
    const std::vector<std::string> little = {"x86_64", "i386", "arm"}, big = {"mips", "mips64be", "m68k", "s390x", "ppc64"};
    for (auto& t : ilp32) {
        CHECK_MESSAGE(failed(by[t]["ptr_is_32"], "INT-DIV-ZERO"), st(t.c_str(), "ptr_is_32"));
        CHECK_MESSAGE(failed(by[t]["mul_big"], "INT-SIGNED-OVF"), st(t.c_str(), "mul_big"));
    }
    for (auto& t : lp64) {
        CHECK_MESSAGE(proved(by[t]["ptr_is_32"]), st(t.c_str(), "ptr_is_32"));
        CHECK_MESSAGE(proved(by[t]["mul_big"]), st(t.c_str(), "mul_big"));
    }
    for (auto& t : little) {
        CHECK_MESSAGE(failed(by[t]["low_first"], "INT-DIV-ZERO"), st(t.c_str(), "low_first"));
        CHECK_MESSAGE(proved(by[t]["high_first"]), st(t.c_str(), "high_first"));
        CHECK_MESSAGE(proved(by[t]["short_b1"]), st(t.c_str(), "short_b1"));
        CHECK_MESSAGE(failed(by[t]["glob_b0"], "INT-DIV-ZERO"), st(t.c_str(), "glob_b0"));
        CHECK_MESSAGE(proved(by[t]["assemble"]), st(t.c_str(), "assemble"));
    }
    for (auto& t : big) {
        CHECK_MESSAGE(proved(by[t]["low_first"]), st(t.c_str(), "low_first"));
        CHECK_MESSAGE(failed(by[t]["high_first"], "INT-DIV-ZERO"), st(t.c_str(), "high_first"));
        CHECK_MESSAGE(failed(by[t]["short_b1"], "INT-DIV-ZERO"), st(t.c_str(), "short_b1"));
        CHECK_MESSAGE(proved(by[t]["glob_b0"]), st(t.c_str(), "glob_b0"));
        CHECK_MESSAGE(failed(by[t]["assemble"], "INT-DIV-ZERO"), st(t.c_str(), "assemble"));
    }
    for (auto& t : kTargets) {
        CHECK_MESSAGE(proved(by[t.name]["mul_small"]), st(t.name, "mul_small"));
        CHECK_MESSAGE(proved(by[t.name]["roundtrip"]), st(t.name, "roundtrip"));
        CHECK_MESSAGE(proved(by[t.name]["roundtrip64"]), st(t.name, "roundtrip64"));
        // layout: int at offset 2 only on m68k; double at offset 4 only on i386
        if (std::string(t.name) == "m68k") CHECK_MESSAGE(proved(by[t.name]["int_at2"]), st(t.name, "int_at2"));
        else CHECK_MESSAGE(failed(by[t.name]["int_at2"], "INT-DIV-ZERO"), st(t.name, "int_at2"));
        if (std::string(t.name) == "i386") CHECK_MESSAGE(proved(by[t.name]["dbl_b11"]), st(t.name, "dbl_b11"));
        else if (std::string(t.name) != "m68k")  // m68k: double is 2-aligned (offset 2): byte 11 is padding
            CHECK_MESSAGE(failed(by[t.name]["dbl_b11"], "INT-DIV-ZERO"), st(t.name, "dbl_b11"));
    }
    // the kfrag mips form (--target=mipsel... -mabi=64 -EB) is big-endian mips64, not little-endian mips32
    CHECK(extra(by["mips64be"]["low_first"], "target").starts_with("mips64"));
    // big-endian proofs rest on the solver alone: the Lean model is little-endian
    for (auto& t : big) {
        const auto& f = by[t]["low_first"];
        CHECK(f.status != L::PROVED_CERTIFIED);
        CHECK(extra(f, "lean_supported") != "yes");
    }
    // translation validation runs only where lli runs the IR (x86 here)
    CHECK(extra(by["s390x"]["low_first"], "tv").find("not the host's") != std::string::npos);
    fs::remove_all(kDir);
}

TEST_CASE("pir targets: a target PIR's pointer model cannot hold is NOTRUN, never checked as another") {
    if (skip_without_frontend()) return;
    fs::remove_all(kDir);
    fs::create_directories(kDir);
    const auto f = kDir / "avr.c";
    std::ofstream(f, std::ios::binary) << "int avr_fn(int x) { return x; }\n";
    std::ofstream(kDir / "cc.json", std::ios::binary)
        << "[{\"directory\": \"" << kDir.generic_string() << "\", \"file\": \"" << f.generic_string()
        << "\", \"arguments\": [\"clang\", \"--target=avr\", \"-mmcu=atmega328\", \"-c\", \"" << f.generic_string() << "\"]}]";
    auto cfg = prism::default_config();
    cfg.root = kDir;
    cfg.jobs = 1;
    cfg.compile_commands = kDir / "cc.json";
    bool fn_row = false, notrun = false;
    std::string msgs;
    for (auto& r : pp::run_pir({f}, cfg)) {
        msgs += r.status + ": " + r.message + "\n";
        if (r.function && *r.function == "avr_fn") fn_row = true;
        if (r.status == L::NOTRUN && r.message.find("16-bit pointers") != std::string::npos) notrun = true;
    }
    if (!notrun && msgs.find("does not compile") != std::string::npos) {
        MESSAGE("clang has no avr target here: " + msgs);
        fs::remove_all(kDir);
        return;
    }
    CHECK_MESSAGE(notrun, msgs);
    CHECK_FALSE(fn_row);
    fs::remove_all(kDir);
}

TEST_CASE("pir targets: pointer model, byte order and target queries from the datalayout and triple") {
    // no datalayout: the x86-64 LP64 model PIR always assumed (hand-written IR)
    auto lay_of = [](const std::string& dl) {
        pp::ir::Module m;
        m.datalayout = dl;
        return pp::ptr_model_of(m);
    };
    CHECK(lay_of("").w == 64);
    CHECK_FALSE(lay_of("").big);
    CHECK(lay_of("e-m:e-p:32:32-Fi8-i64:64-v128:64:128-a:0:32-n32-S64").w == 32);
    CHECK(lay_of("E-m:e-p:32:16:32-i8:8:8-i16:16:16-i32:16:32-n8:16:32-a:0:16-S16").big);
    CHECK(lay_of("E-m:e-p:32:16:32-i8:8:8-i16:16:16-i32:16:32-n8:16:32-a:0:16-S16").w == 32);
    CHECK(lay_of("E-S64-m:e-i1:8:16-i8:8:16-i64:64-f128:64-v128:64-a:8:16-n32:64").big);
    CHECK(lay_of("E-S64-m:e-i1:8:16-i8:8:16-i64:64-f128:64-v128:64-a:8:16-n32:64").w == 64);
    pp::ir::Module avr;
    avr.datalayout = "e-P1-p:16:8-i8:8-i16:8-i32:8-i64:8-f32:8-f64:8-n8-a:8";
    CHECK(pp::target_unsupported(avr).find("16-bit pointers") != std::string::npos);
    pp::ir::Module arm;
    arm.triple = "armv6k-unknown-linux-gnueabi";
    CHECK_FALSE(pp::target_x86_64(arm));
    CHECK(pp::target_long_double(arm) == "double");
    pp::ir::Module none;
    CHECK(pp::target_x86_64(none));
}

#endif
