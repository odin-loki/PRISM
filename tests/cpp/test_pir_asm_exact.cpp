// Exact x86-64 inline-asm semantics in the PIR translator (W4, translate_asm.inc).
//
// 1. Translation validation: for every modelled template the asm is written as LLVM IR,
//    translated with --kernel-api (the exact model), run by the PIR interpreter on random
//    vectors, and compared with the same instruction executed natively on this host.
//    Needs x86-64 and GNU inline asm; the per-template agreement counts are printed
//    and listed in docs/ASM_CENSUS.md.
// 2. Table-driven verdicts on testdata/pir/kernel_asm_exact.c (a true and a false variant
//    of each template): *_ok PROVED-ASSUMING asm-exact, *_bad FAILED.
#include <doctest/doctest.h>
#ifdef ERROR
#  undef ERROR
#endif

#include "prism/config.hpp"
#include "prism/laws.hpp"
#include "prism/pir.hpp"
#include "prism/pir/frontend.hpp"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <map>
#include <random>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;
namespace pp = prism::pir;

#if defined(__x86_64__) && (defined(__clang__) || defined(__GNUC__))

using u64 = std::uint64_t;
using u32 = std::uint32_t;
using Nat = std::function<u64(u64, u64, u64)>;

struct Case {
    std::string name;
    std::string body;  // IR using %a %b %c (i64), must define %r (i64)
    Nat nat;
};

const char* kTail = ",~{dirflag},~{fpsr},~{flags}";

std::string ir_of(const Case& c) {
    return "define i64 @f(i64 %a, i64 %b, i64 %c) {\nentry:\n" + c.body + "  ret i64 %r\n}\n";
}

// ---- IR builders ----
std::string un64(const std::string& tmpl, const std::string& cons, const std::string& arg = "%a") {
    return "  %r = call i64 asm \"" + tmpl + "\", \"" + cons + kTail + "\"(i64 " + arg + ")\n";
}
std::string un32(const std::string& tmpl, const std::string& cons) {
    return "  %t = trunc i64 %a to i32\n  %x = call i32 asm \"" + tmpl + "\", \"" + cons + kTail +
           "\"(i32 %t)\n  %r = zext i32 %x to i64\n";
}
std::string shift_ir(const std::string& mn, int w) {
    const std::string ty = "i" + std::to_string(w);
    std::string s = "  %cc = trunc i64 %c to i32\n";
    if (w == 32) s += "  %t = trunc i64 %a to i32\n";
    const std::string av = w == 32 ? "%t" : "%a";
    s += "  %x = call " + ty + " asm \"" + mn + " %cl, $0\", \"=r,{cx},0,~{cc}" + kTail + "\"(i32 %cc, " + ty + " " + av + ")\n";
    s += w == 32 ? "  %r = zext i32 %x to i64\n" : "  %r = add i64 %x, 0\n";
    return s;
}
// two-operand ALU: value or flag
std::string alu_ir(const std::string& insn, const std::string& cc, bool flag) {
    std::string s = "  %t = call { i64, i32 } asm \"" + insn + " $2, $0\", \"=r,={@cc" + cc + "},r,0" + kTail + "\"(i64 %b, i64 %a)\n";
    s += flag ? "  %f = extractvalue { i64, i32 } %t, 1\n  %r = zext i32 %f to i64\n" : "  %r = extractvalue { i64, i32 } %t, 0\n";
    return s;
}
std::string cmp_ir(const std::string& cc) {
    return "  %f = call i32 asm \"cmpq $2, $1\", \"={@cc" + cc + "},r,r" + kTail + "\"(i64 %a, i64 %b)\n  %r = zext i32 %f to i64\n";
}

template <class F>
Nat nat3(F f) { return Nat(f); }

#define ALU_NATIVE(NAME, INSN, CC, FLAG)                                                       \
    Nat NAME() {                                                                               \
        return [](u64 a, u64 b, u64) -> u64 {                                                  \
            u64 x = a;                                                                         \
            unsigned char f;                                                                   \
            asm(INSN " %2, %0" : "+r"(x), "=@cc" CC(f) : "r"(b));                              \
            return FLAG ? (u64)f : x;                                                          \
        };                                                                                     \
    }
ALU_NATIVE(n_add_v, "addq", "c", false)
ALU_NATIVE(n_add_c, "addq", "c", true)
ALU_NATIVE(n_add_o, "addq", "o", true)
ALU_NATIVE(n_add_z, "addq", "z", true)
ALU_NATIVE(n_add_s, "addq", "s", true)
ALU_NATIVE(n_sub_v, "subq", "c", false)
ALU_NATIVE(n_sub_c, "subq", "c", true)
ALU_NATIVE(n_sub_o, "subq", "o", true)
ALU_NATIVE(n_sub_z, "subq", "z", true)
ALU_NATIVE(n_sub_s, "subq", "s", true)
ALU_NATIVE(n_and_v, "andq", "z", false)
ALU_NATIVE(n_and_z, "andq", "z", true)
ALU_NATIVE(n_or_v, "orq", "s", false)
ALU_NATIVE(n_xor_v, "xorq", "z", false)
ALU_NATIVE(n_xor_z, "xorq", "z", true)

#define CMP_NATIVE(NAME, CC)                                                   \
    Nat NAME() {                                                               \
        return [](u64 a, u64 b, u64) -> u64 {                                  \
            unsigned char f;                                                   \
            asm("cmpq %2, %1" : "=@cc" CC(f) : "r"(a), "r"(b));                \
            return f;                                                          \
        };                                                                     \
    }
CMP_NATIVE(n_cmp_l, "l")
CMP_NATIVE(n_cmp_b, "b")
CMP_NATIVE(n_cmp_a, "a")
CMP_NATIVE(n_cmp_le, "le")
CMP_NATIVE(n_cmp_g, "g")
CMP_NATIVE(n_cmp_e, "e")

#define SHIFT_NATIVE(NAME, INSN, T)                                         \
    Nat NAME() {                                                            \
        return [](u64 a, u64, u64 c) -> u64 {                               \
            T x = (T)a;                                                     \
            unsigned cl = (unsigned)c;                                      \
            asm(INSN " %%cl, %0" : "+r"(x) : "c"(cl) : "cc");               \
            return (u64)x;                                                  \
        };                                                                  \
    }
SHIFT_NATIVE(n_rolq, "rolq", u64)
SHIFT_NATIVE(n_rorq, "rorq", u64)
SHIFT_NATIVE(n_shlq, "shlq", u64)
SHIFT_NATIVE(n_shrq, "shrq", u64)
SHIFT_NATIVE(n_sarq, "sarq", std::int64_t)
SHIFT_NATIVE(n_roll, "roll", u32)
SHIFT_NATIVE(n_rorl, "rorl", u32)
SHIFT_NATIVE(n_shll, "shll", u32)
SHIFT_NATIVE(n_shrl, "shrl", u32)
SHIFT_NATIVE(n_sarl, "sarl", std::int32_t)

#define BITOP_NATIVE(NAME, INSN, T, FLAG)                                          \
    Nat NAME() {                                                                   \
        return [](u64 a, u64 b, u64) -> u64 {                                      \
            T x = (T)a, bit = (T)b;                                                \
            unsigned char f;                                                       \
            asm(INSN " %2, %0" : "+r"(x), "=@ccc"(f) : "r"(bit));                  \
            return FLAG ? (u64)f : (u64)x;                                         \
        };                                                                         \
    }
BITOP_NATIVE(n_bts_v, "btsq", u64, false)
BITOP_NATIVE(n_bts_f, "btsq", u64, true)
BITOP_NATIVE(n_btr_v, "btrq", u64, false)
BITOP_NATIVE(n_btc_v, "btcq", u64, false)
BITOP_NATIVE(n_btc_f, "btcq", u64, true)
BITOP_NATIVE(n_btsl_v, "btsl", u32, false)
BITOP_NATIVE(n_btrl_f, "btrl", u32, true)

bool cpu_has(const char* flag) {
    std::FILE* f = std::fopen("/proc/cpuinfo", "r");
    if (!f) return false;
    char line[8192];
    bool ok = false;
    while (std::fgets(line, sizeof line, f)) {
        std::string s(line);
        if (s.rfind("flags", 0) == 0) {
            ok = s.find(std::string(" ") + flag) != std::string::npos;
            break;
        }
    }
    std::fclose(f);
    return ok;
}

std::vector<Case> make_cases() {
    std::vector<Case> v;
    // bswap
    v.push_back({"bswapq", un64("bswapq $0", "=r,0"), [](u64 a, u64, u64) { asm("bswapq %0" : "+r"(a)); return a; }});
    v.push_back({"bswapl", un32("bswapl $0", "=r,0"), [](u64 a, u64, u64) { u32 x = (u32)a; asm("bswapl %0" : "+r"(x)); return (u64)x; }});
    // popcnt / lzcnt / tzcnt / bsf / bsr
    v.push_back({"popcntq", un64("popcntq $1, $0", "=r,r,~{cc}"), [](u64 a, u64, u64) { u64 r; asm("popcntq %1, %0" : "=r"(r) : "r"(a) : "cc"); return r; }});
    v.push_back({"popcntl", un32("popcntl $1, $0", "=r,r,~{cc}"), [](u64 a, u64, u64) { u32 r, x = (u32)a; asm("popcntl %1, %0" : "=r"(r) : "r"(x) : "cc"); return (u64)r; }});
    if (cpu_has("abm") || cpu_has("lzcnt"))
        v.push_back({"lzcntq", un64("lzcntq $1, $0", "=r,r,~{cc}"), [](u64 a, u64, u64) { u64 r; asm("lzcntq %1, %0" : "=r"(r) : "r"(a) : "cc"); return r; }});
    if (cpu_has("bmi1"))
        v.push_back({"tzcntq", un64("tzcntq $1, $0", "=r,r,~{cc}"), [](u64 a, u64, u64) { u64 r; asm("tzcntq %1, %0" : "=r"(r) : "r"(a) : "cc"); return r; }});
    const std::string nz = "  %z = icmp eq i64 %a, 0\n  %n = select i1 %z, i64 1, i64 %a\n";
    v.push_back({"bsfq", nz + un64("bsfq $1, $0", "=r,r,~{cc}", "%n"), [](u64 a, u64, u64) { if (!a) a = 1; u64 r; asm("bsfq %1, %0" : "=r"(r) : "r"(a) : "cc"); return r; }});
    v.push_back({"bsrq", nz + un64("bsrq $1, $0", "=r,r,~{cc}", "%n"), [](u64 a, u64, u64) { if (!a) a = 1; u64 r; asm("bsrq %1, %0" : "=r"(r) : "r"(a) : "cc"); return r; }});
    // rotates and shifts by %cl
    v.push_back({"rolq", shift_ir("rolq", 64), n_rolq()});
    v.push_back({"rorq", shift_ir("rorq", 64), n_rorq()});
    v.push_back({"shlq", shift_ir("shlq", 64), n_shlq()});
    v.push_back({"shrq", shift_ir("shrq", 64), n_shrq()});
    v.push_back({"sarq", shift_ir("sarq", 64), n_sarq()});
    v.push_back({"roll", shift_ir("roll", 32), n_roll()});
    v.push_back({"rorl", shift_ir("rorl", 32), n_rorl()});
    v.push_back({"shll", shift_ir("shll", 32), n_shll()});
    v.push_back({"shrl", shift_ir("shrl", 32), n_shrl()});
    v.push_back({"sarl", shift_ir("sarl", 32), n_sarl()});
    // ALU with flag outputs
    v.push_back({"addq value", alu_ir("addq", "c", false), n_add_v()});
    v.push_back({"addq CF", alu_ir("addq", "c", true), n_add_c()});
    v.push_back({"addq OF", alu_ir("addq", "o", true), n_add_o()});
    v.push_back({"addq ZF", alu_ir("addq", "z", true), n_add_z()});
    v.push_back({"addq SF", alu_ir("addq", "s", true), n_add_s()});
    v.push_back({"subq value", alu_ir("subq", "c", false), n_sub_v()});
    v.push_back({"subq CF", alu_ir("subq", "c", true), n_sub_c()});
    v.push_back({"subq OF", alu_ir("subq", "o", true), n_sub_o()});
    v.push_back({"subq ZF", alu_ir("subq", "z", true), n_sub_z()});
    v.push_back({"subq SF", alu_ir("subq", "s", true), n_sub_s()});
    v.push_back({"andq value", alu_ir("andq", "z", false), n_and_v()});
    v.push_back({"andq ZF", alu_ir("andq", "z", true), n_and_z()});
    v.push_back({"orq value", alu_ir("orq", "s", false), n_or_v()});
    v.push_back({"xorq value", alu_ir("xorq", "z", false), n_xor_v()});
    v.push_back({"xorq ZF", alu_ir("xorq", "z", true), n_xor_z()});
    v.push_back({"cmpq l", cmp_ir("l"), n_cmp_l()});
    v.push_back({"cmpq b", cmp_ir("b"), n_cmp_b()});
    v.push_back({"cmpq a", cmp_ir("a"), n_cmp_a()});
    v.push_back({"cmpq le", cmp_ir("le"), n_cmp_le()});
    v.push_back({"cmpq g", cmp_ir("g"), n_cmp_g()});
    v.push_back({"cmpq e", cmp_ir("e"), n_cmp_e()});
    // 128-bit add: addq; adcq (carry from the first instruction)
    v.push_back({"addq+adcq", "  %d = xor i64 %a, %b\n  %t = call { i64, i64 } asm \"addq $4, $0; adcq $5, $1\", \"=r,=r,0,1,r,r,~{cc}" + std::string(kTail) +
                                  "\"(i64 %a, i64 %b, i64 %c, i64 %d)\n  %h = extractvalue { i64, i64 } %t, 1\n  %l = extractvalue { i64, i64 } %t, 0\n  %m = mul i64 %l, 11400714819323198485\n  %r = add i64 %h, %m\n",
                 [](u64 a, u64 b, u64 c) { u64 d = a ^ b, lo = a, hi = b; asm("addq %2, %0; adcq %3, %1" : "+r"(lo), "+r"(hi) : "r"(c), "r"(d) : "cc"); (void)0; return hi + lo * 11400714819323198485ull; }});
    v.push_back({"subq+sbbq", "  %d = xor i64 %a, %b\n  %t = call { i64, i64 } asm \"subq $4, $0; sbbq $5, $1\", \"=r,=r,0,1,r,r,~{cc}" + std::string(kTail) +
                                  "\"(i64 %a, i64 %b, i64 %c, i64 %d)\n  %h = extractvalue { i64, i64 } %t, 1\n  %l = extractvalue { i64, i64 } %t, 0\n  %m = mul i64 %l, 11400714819323198485\n  %r = add i64 %h, %m\n",
                 [](u64 a, u64 b, u64 c) { u64 d = a ^ b, lo = a, hi = b; asm("subq %2, %0; sbbq %3, %1" : "+r"(lo), "+r"(hi) : "r"(c), "r"(d) : "cc"); return hi + lo * 11400714819323198485ull; }});
    // wide multiply
    auto mulir = [](const char* insn, int w) {
        const std::string ty = "i" + std::to_string(w);
        std::string s;
        std::string av = "%a", bv = "%b";
        if (w == 32) s = "  %a32 = trunc i64 %a to i32\n  %b32 = trunc i64 %b to i32\n", av = "%a32", bv = "%b32";
        s += "  %t = call { " + ty + ", " + ty + " } asm \"" + insn + " $3\", \"={ax},={dx},0,rm,~{cc}" + kTail + "\"(" + ty + " " + av + ", " + ty + " " + bv + ")\n";
        s += "  %l0 = extractvalue { " + ty + ", " + ty + " } %t, 0\n  %h0 = extractvalue { " + ty + ", " + ty + " } %t, 1\n";
        if (w == 32)
            s += "  %l = zext i32 %l0 to i64\n  %h = zext i32 %h0 to i64\n";
        else
            s += "  %l = add i64 %l0, 0\n  %h = add i64 %h0, 0\n";
        s += "  %m = mul i64 %l, 11400714819323198485\n  %r = add i64 %h, %m\n";
        return s;
    };
    v.push_back({"mulq", mulir("mulq", 64), [](u64 a, u64 b, u64) { u64 lo = a, hi; asm("mulq %2" : "+a"(lo), "=d"(hi) : "rm"(b) : "cc"); return hi + lo * 11400714819323198485ull; }});
    v.push_back({"imulq", mulir("imulq", 64), [](u64 a, u64 b, u64) { u64 lo = a, hi; asm("imulq %2" : "+a"(lo), "=d"(hi) : "rm"(b) : "cc"); return hi + lo * 11400714819323198485ull; }});
    v.push_back({"mull", mulir("mull", 32), [](u64 a, u64 b, u64) { u32 lo = (u32)a, hi; u32 y = (u32)b; asm("mull %2" : "+a"(lo), "=d"(hi) : "rm"(y) : "cc"); return (u64)hi + (u64)lo * 11400714819323198485ull; }});
    v.push_back({"imull", mulir("imull", 32), [](u64 a, u64 b, u64) { u32 lo = (u32)a, hi; u32 y = (u32)b; asm("imull %2" : "+a"(lo), "=d"(hi) : "rm"(y) : "cc"); return (u64)hi + (u64)lo * 11400714819323198485ull; }});
    v.push_back({"imulq two-operand", "  %t = call { i64, i32 } asm \"imulq $2, $0\", \"=r,={@ccc},r,0" + std::string(kTail) + "\"(i64 %b, i64 %a)\n  %v = extractvalue { i64, i32 } %t, 0\n  %f = extractvalue { i64, i32 } %t, 1\n  %fz = zext i32 %f to i64\n  %m = mul i64 %v, 3\n  %r = add i64 %m, %fz\n",
                 [](u64 a, u64 b, u64) { u64 x = a; unsigned char f; asm("imulq %2, %0" : "+r"(x), "=@ccc"(f) : "r"(b)); return x * 3 + f; }});
    // 32-bit divide (edx:eax / src)
    v.push_back({"divl", "  %s0 = trunc i64 %b to i32\n  %s = or i32 %s0, 1\n  %e = trunc i64 %a to i32\n  %c0 = trunc i64 %c to i32\n  %d = urem i32 %c0, %s\n"
                         "  %t = call { i32, i32 } asm \"divl $4\", \"={ax},={dx},0,1,rm,~{cc}" + std::string(kTail) + "\"(i32 %e, i32 %d, i32 %s)\n"
                         "  %q = extractvalue { i32, i32 } %t, 0\n  %rm = extractvalue { i32, i32 } %t, 1\n  %q64 = zext i32 %q to i64\n  %r64 = zext i32 %rm to i64\n  %m = mul i64 %q64, 4294967311\n  %r = add i64 %m, %r64\n",
                 [](u64 a, u64 b, u64 c) { u32 s = (u32)b | 1, e = (u32)a, d = (u32)c % s; u32 q = e, rr = d; asm("divl %2" : "+a"(q), "+d"(rr) : "rm"(s) : "cc"); return (u64)q * 4294967311ull + rr; }});
    v.push_back({"idivl", "  %s0 = trunc i64 %b to i32\n  %s1 = or i32 %s0, 1\n  %m1 = icmp eq i32 %s1, -1\n  %s = select i1 %m1, i32 3, i32 %s1\n  %e = trunc i64 %a to i32\n  %d = ashr i32 %e, 31\n"
                          "  %t = call { i32, i32 } asm \"idivl $4\", \"={ax},={dx},0,1,rm,~{cc}" + std::string(kTail) + "\"(i32 %e, i32 %d, i32 %s)\n"
                          "  %q = extractvalue { i32, i32 } %t, 0\n  %rm = extractvalue { i32, i32 } %t, 1\n  %q64 = zext i32 %q to i64\n  %r64 = zext i32 %rm to i64\n  %m = mul i64 %q64, 4294967311\n  %r = add i64 %m, %r64\n",
                 [](u64 a, u64 b, u64) { std::int32_t s = (std::int32_t)((u32)b | 1); if (s == -1) s = 3; std::int32_t e = (std::int32_t)a, d = e >> 31, q = e, rr = d; asm("idivl %2" : "+a"(q), "+d"(rr) : "rm"(s) : "cc"); return (u64)(u32)q * 4294967311ull + (u32)rr; }});
    // bit test and modify (register, register bit number)
    auto bt_ir = [](const char* insn, int w, bool flag) {
        const std::string ty = "i" + std::to_string(w);
        std::string s;
        std::string av = "%a", bv = "%b";
        if (w == 32) s = "  %a32 = trunc i64 %a to i32\n  %b32 = trunc i64 %b to i32\n", av = "%a32", bv = "%b32";
        s += "  %t = call { " + ty + ", i32 } asm \"" + insn + " $2, $0\", \"=r,={@ccc},r,0" + kTail + "\"(" + ty + " " + bv + ", " + ty + " " + av + ")\n";
        s += flag ? "  %f = extractvalue { " + ty + ", i32 } %t, 1\n  %r = zext i32 %f to i64\n"
                  : "  %v = extractvalue { " + ty + ", i32 } %t, 0\n" + (w == 32 ? std::string("  %r = zext i32 %v to i64\n") : std::string("  %r = add i64 %v, 0\n"));
        return s;
    };
    v.push_back({"btsq value", bt_ir("btsq", 64, false), n_bts_v()});
    v.push_back({"btsq CF", bt_ir("btsq", 64, true), n_bts_f()});
    v.push_back({"btrq value", bt_ir("btrq", 64, false), n_btr_v()});
    v.push_back({"btcq value", bt_ir("btcq", 64, false), n_btc_v()});
    v.push_back({"btcq CF", bt_ir("btcq", 64, true), n_btc_f()});
    v.push_back({"btsl value", bt_ir("btsl", 32, false), n_btsl_v()});
    v.push_back({"btrl CF", bt_ir("btrl", 32, true), n_btrl_f()});
    v.push_back({"btq CF (no write)", "  %f = call i32 asm \"btq $2, $1\", \"={@ccc},r,r" + std::string(kTail) + "\"(i64 %a, i64 %b)\n  %r = zext i32 %f to i64\n",
                 [](u64 a, u64 b, u64) { unsigned char f; asm("btq %2, %1" : "=@ccc"(f) : "r"(a), "r"(b)); return (u64)f; }});
    // register xchg and the lock-prefixed memory forms (one memory operand, sequential read-modify-write)
    const std::string mem_pre = "  %m = alloca i64\n  store i64 %a, ptr %m\n";
    const std::string mem_post = "  %v = load i64, ptr %m\n  %k = mul i64 %old, 11400714819323198485\n  %r = add i64 %v, %k\n";
    v.push_back({"lock xaddq", mem_pre + "  %old = call i64 asm sideeffect \"lock; xaddq $0, $1\", \"=r,=*m,0,*m,~{memory},~{cc}" + kTail +
                                   "\"(ptr elementtype(i64) %m, i64 %b, ptr elementtype(i64) %m)\n" + mem_post,
                 [](u64 a, u64 b, u64) { u64 m = a, o = b; asm volatile("lock; xaddq %0, %1" : "+r"(o), "+m"(m) : : "memory", "cc"); return m + o * 11400714819323198485ull; }});
    v.push_back({"xchgq", mem_pre + "  %old = call i64 asm sideeffect \"xchgq $0, $1\", \"=r,=*m,0,*m,~{memory}" + std::string(kTail) +
                              "\"(ptr elementtype(i64) %m, i64 %b, ptr elementtype(i64) %m)\n" + mem_post,
                 [](u64 a, u64 b, u64) { u64 m = a, o = b; asm volatile("xchgq %0, %1" : "+r"(o), "+m"(m) : : "memory"); return m + o * 11400714819323198485ull; }});
    v.push_back({"lock cmpxchgq", "  %e = select i1 true, i64 %a, i64 %a\n" + mem_pre + "  %x = and i64 %c, 1\n  %eq = icmp eq i64 %x, 0\n  %exp = select i1 %eq, i64 %a, i64 %b\n"
                                      "  %old = call i64 asm sideeffect \"lock; cmpxchgq $2,$1\", \"={ax},=*m,r,0,*m,~{memory}" + std::string(kTail) +
                                      "\"(ptr elementtype(i64) %m, i64 %c, i64 %exp, ptr elementtype(i64) %m)\n" + mem_post,
                 [](u64 a, u64 b, u64 c) { u64 m = a, exp = (c & 1) == 0 ? a : b, prev = exp; asm volatile("lock; cmpxchgq %2,%1" : "+a"(prev), "+m"(m) : "r"(c) : "memory"); return m + prev * 11400714819323198485ull; }});
    v.push_back({"lock addq (memory only)", mem_pre + "  call void asm sideeffect \"lock; addq $1, $0\", \"=*m,er,*m,~{memory},~{cc}" + std::string(kTail) +
                                                "\"(ptr elementtype(i64) %m, i64 %b, ptr elementtype(i64) %m)\n  %r = load i64, ptr %m\n",
                 [](u64 a, u64 b, u64) { u64 m = a; asm volatile("lock; addq %1, %0" : "+m"(m) : "er"(b) : "memory", "cc"); return m; }});
    v.push_back({"lock btsq CF (constant bit, memory)", mem_pre + "  %t = call i32 asm sideeffect \"lock; btsq $$5, $1\", \"={@ccc},=*m,*m,~{memory}" + std::string(kTail) +
                                                           "\"(ptr elementtype(i64) %m, ptr elementtype(i64) %m)\n  %v = load i64, ptr %m\n  %tz = zext i32 %t to i64\n  %r = add i64 %v, %tz\n",
                 [](u64 a, u64, u64) { u64 m = a; unsigned char f; asm volatile("lock; btsq $5, %1" : "=@ccc"(f), "+m"(m) : : "memory"); return m + f; }});
    return v;
}

TEST_CASE("asm exact: every modelled x86-64 template agrees with native execution (PIR interpreter vs host)") {
    auto cases = make_cases();
    std::mt19937_64 rng(20261120);
    constexpr int kVectors = 1200;
    std::size_t total = 0;
    for (auto& c : cases) {
        const std::string text = ir_of(c);
        auto m = pp::ir::parse_module(text);
        const auto* f = m.find("f");
        REQUIRE(f != nullptr);
        pp::TranslateOptions o;
        o.kernel_api = "linux";
        auto t = pp::translate(m, *f, o);
        INFO(c.name << ": " << t.reason << "\n" << text);
        REQUIRE(t.fn.has_value());
        bool exact = false;
        for (auto& a : t.fn->assumptions)
            if (a.rfind("asm-exact", 0) == 0) exact = true;
        CHECK_MESSAGE(exact, c.name << " is not modelled exactly (fell back to havoc)");
        int agree = 0, bad = 0;
        for (int i = 0; i < kVectors; ++i) {
            u64 a = rng(), b = rng(), cc = rng();
            switch (i % 8) {  // edge values
                case 0: a = 0; break;
                case 1: b = ~u64{0}; break;
                case 2: a = u64{1} << 63; break;
                case 3: b = a; break;
                default: break;
            }
            auto r = pp::interpret(*t.fn, {a, b, cc});
            const u64 want = c.nat(a, b, cc);
            if (r.status == pp::InterpResult::Returned && r.ret == want) {
                ++agree;
            } else if (bad++ < 3) {
                MESSAGE(c.name << ": a=" << a << " b=" << b << " c=" << cc << " status " << r.status << " model " << r.ret << " native " << want);
            }
        }
        CHECK_MESSAGE(bad == 0, c.name << ": " << bad << " of " << kVectors << " vectors disagree");
        std::printf("asm-exact %-40s %d/%d vectors agree\n", c.name.c_str(), agree, kVectors);
        total += static_cast<std::size_t>(agree);
    }
    std::printf("asm-exact total %zu agreeing vectors over %zu templates\n", total, cases.size());
}

#endif  // x86-64 GNU asm

pp::Translation tr_ir(const std::string& body_ir, const std::string& name) {
    auto m = pp::ir::parse_module(body_ir);
    const auto* f = m.find(name);
    REQUIRE(f != nullptr);
    pp::TranslateOptions o;
    o.kernel_api = "linux";
    return pp::translate(m, *f, o);
}

bool has_assumption(const pp::Translation& t, const std::string& pre) {
    if (!t.fn) return false;
    for (auto& a : t.fn->assumptions)
        if (a.rfind(pre, 0) == 0) return true;
    return false;
}

}  // namespace

TEST_CASE("asm exact: fences are exact no-ops, rdtsc is nondeterministic, privileged instructions stay havoc under asm-privileged") {
    const std::string tail = ",~{dirflag},~{fpsr},~{flags}\"()";
    auto fence = tr_ir("define i64 @f(i64 %a) {\nentry:\n  call void asm sideeffect \"mfence\", \"~{memory}" + tail + "\n  ret i64 %a\n}\n", "f");
    REQUIRE(fence.fn.has_value());
    CHECK(has_assumption(fence, "asm-exact"));
    CHECK_FALSE(has_assumption(fence, "asm-havoc"));
    auto tsc = tr_ir("define i64 @f() {\nentry:\n  %t = call { i32, i32 } asm sideeffect \"rdtsc\", \"={ax},={dx}" + tail + "\n  %l = extractvalue { i32, i32 } %t, 0\n  %r = zext i32 %l to i64\n  ret i64 %r\n}\n", "f");
    REQUIRE(tsc.fn.has_value());
    CHECK(has_assumption(tsc, "asm-nondet"));
    for (const char* priv : {"cli", "sti", "hlt", "cpuid", "wrmsr", "rdmsr", "mov %rax, %cr3"}) {
        auto t = tr_ir(std::string("define void @f() {\nentry:\n  call void asm sideeffect \"") + priv + "\", \"~{memory}" + tail + "\n  ret void\n}\n", "f");
        REQUIRE(t.fn.has_value());
        CHECK_MESSAGE(has_assumption(t, "asm-privileged"), priv);
        CHECK_FALSE(has_assumption(t, "asm-exact"));
    }
    // an unknown template keeps the havoc model
    auto unk = tr_ir("define i64 @f(i64 %a) {\nentry:\n  %r = call i64 asm \"vpshufb $1, $0\", \"=r,r" + tail.substr(0, tail.size() - 3) + "\"(i64 %a)\n  ret i64 %r\n}\n", "f");
    REQUIRE(unk.fn.has_value());
    CHECK(has_assumption(unk, "asm-havoc"));
    CHECK_FALSE(has_assumption(unk, "asm-exact"));
}

namespace {

struct Row {
    const char* function;
    const char* status;
    const char* tag;
};

}  // namespace

TEST_CASE("asm exact: C templates, a true and a false variant of each (kernel_asm_exact.c)") {
    auto cfg = prism::default_config();
    std::string why;
    if (!pp::frontend_usable(cfg, &why)) {
        MESSAGE(why);
        return;
    }
    if (!pp::z3_available()) {
        MESSAGE("NOTRUN: pir asm tests need Z3");
        return;
    }
    auto dir = fs::path(PRISM_SOURCE_DIR) / "testdata" / "pir";
    cfg.root = dir;
    cfg.jobs = 2;
    cfg.pir_kernel_api = "linux";
    auto out = pp::run_pir({dir / "kernel_asm_exact.c"}, cfg);
    std::map<std::string, prism::Finding> got;
    for (auto& f : out)
        if (f.function) got[*f.function] = f;
    const std::vector<Row> rows = {
        {"bswap_ok", "PROVED-ASSUMING", "asm-exact"},        {"bswap_bad", "FAILED", ""},
        {"popcnt_ok", "PROVED-ASSUMING", "asm-exact"},       {"popcnt_bad", "FAILED", ""},
        {"bsf_ok", "PROVED-ASSUMING", "asm-exact"},          {"bsf_bad", "FAILED", ""},
        {"rotate_ok", "PROVED-ASSUMING", "asm-exact"},       {"rotate_bad", "FAILED", ""},
        {"carry_ok", "PROVED-ASSUMING", "asm-exact"},        {"carry_bad", "FAILED", ""},
        {"adc128_ok", "PROVED-ASSUMING", "asm-exact"},       {"adc128_bad", "FAILED", ""},
        {"bts_ok", "PROVED-ASSUMING", "asm-exact"},          {"bts_bad", "FAILED", ""},
        {"xadd_ok", "PROVED-ASSUMING", "asm-exact"},         {"xadd_bad", "FAILED", ""},
        {"cmpxchg_ok", "PROVED-ASSUMING", "asm-exact"},      {"cmpxchg_bad", "FAILED", ""},
        {"xchg_ok", "PROVED-ASSUMING", "asm-exact"},         {"xchg_bad", "FAILED", ""},
        {"mulhi_ok", "PROVED-ASSUMING", "asm-exact"},        {"mulhi_bad", "FAILED", ""},
        {"div_ok", "PROVED-ASSUMING", "asm-exact"},          {"div_bad", "FAILED", ""},
        {"fence_ok", "PROVED-ASSUMING", "asm-exact"},        {"fence_bad", "FAILED", ""},
        {"rdtsc_ok", "PROVED-ASSUMING", "asm-nondet"},       {"rdtsc_bad", "FAILED", ""},
        {"cpuid_ok", "PROVED-ASSUMING", "asm-privileged"},   {"cpuid_bad", "FAILED", ""},
    };
    std::vector<std::string> wrong;
    for (auto& r : rows) {
        auto it = got.find(r.function);
        if (it == got.end()) {
            wrong.push_back(std::string(r.function) + " missing");
            continue;
        }
        auto& f = it->second;
        if (f.status != r.status) {
            wrong.push_back(std::string(r.function) + " status " + f.status + " != " + r.status + " (" + f.message + ")");
            continue;
        }
        auto ai = f.extra.find("assumptions");
        const std::string as = ai == f.extra.end() ? std::string() : ai->second;
        if (f.status == prism::laws::PROVED_ASSUMING && r.tag[0] && as.find(r.tag) == std::string::npos)
            wrong.push_back(std::string(r.function) + " assumptions lack '" + r.tag + "': " + as);
    }
    for (auto& w : wrong) MESSAGE(w);
    CHECK(wrong.empty());
}
