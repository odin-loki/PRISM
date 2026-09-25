// PIR core: operator names, the textual form (hashed into extra.pir_hash),
// SHA-256, and the concrete interpreter used by translation validation.

#include "prism/pir.hpp"

#include "fp.hpp"
#include "memory.hpp"

#include <array>
#include <cstdio>
#include <sstream>

namespace prism::pir {

Arg Arg::c(unsigned w, uint64_t v) {
    Arg a;
    a.is_const = true;
    a.width = w;
    a.bits = w >= 64 ? v : (v & ((uint64_t{1} << w) - 1));
    return a;
}

Arg Arg::v(int var, unsigned w) {
    Arg a;
    a.var = var;
    a.width = w;
    return a;
}

const char* op_name(Op op) {
    switch (op) {
        case Op::Copy: return "copy";
        case Op::Havoc: return "havoc";
        case Op::Add: return "add";
        case Op::Sub: return "sub";
        case Op::Mul: return "mul";
        case Op::UDiv: return "udiv";
        case Op::SDiv: return "sdiv";
        case Op::URem: return "urem";
        case Op::SRem: return "srem";
        case Op::Shl: return "shl";
        case Op::LShr: return "lshr";
        case Op::AShr: return "ashr";
        case Op::And: return "and";
        case Op::Or: return "or";
        case Op::Xor: return "xor";
        case Op::Eq: return "eq";
        case Op::Ne: return "ne";
        case Op::Ult: return "ult";
        case Op::Ule: return "ule";
        case Op::Ugt: return "ugt";
        case Op::Uge: return "uge";
        case Op::Slt: return "slt";
        case Op::Sle: return "sle";
        case Op::Sgt: return "sgt";
        case Op::Sge: return "sge";
        case Op::Select: return "select";
        case Op::ZExt: return "zext";
        case Op::SExt: return "sext";
        case Op::Trunc: return "trunc";
        case Op::SMax: return "smax";
        case Op::SMin: return "smin";
        case Op::UMax: return "umax";
        case Op::UMin: return "umin";
        case Op::Abs: return "abs";
        case Op::Ctlz: return "ctlz";
        case Op::Cttz: return "cttz";
        case Op::Ctpop: return "ctpop";
        case Op::Bswap: return "bswap";
        case Op::SAddOvf: return "sadd.ovf";
        case Op::SSubOvf: return "ssub.ovf";
        case Op::SMulOvf: return "smul.ovf";
        case Op::UAddOvf: return "uadd.ovf";
        case Op::USubOvf: return "usub.ovf";
        case Op::UMulOvf: return "umul.ovf";
        case Op::SDivOvf: return "sdiv.ovf";
        case Op::ShiftOob: return "shift.oob";
        case Op::ShlSOvf: return "shl.signed.ovf";
        case Op::ShlNswOvf: return "shl.nsw.ovf";
        case Op::ShlNuwOvf: return "shl.nuw.ovf";
        case Op::LostBitsL: return "lshr.inexact";
        case Op::LostBitsA: return "ashr.inexact";
        case Op::InexactU: return "udiv.inexact";
        case Op::InexactS: return "sdiv.inexact";
        case Op::ObjSize: return "obj.size";
        case Op::ObjLive: return "obj.live";
        case Op::ObjKind: return "obj.kind";
        case Op::ObjAlign: return "obj.align";
        case Op::FAdd: return "fadd";
        case Op::FSub: return "fsub";
        case Op::FMul: return "fmul";
        case Op::FDiv: return "fdiv";
        case Op::FRem: return "frem";
        case Op::FSqrt: return "fsqrt";
        case Op::FFma: return "ffma";
        case Op::FMulAdd: return "fmuladd";
        case Op::FMinNum: return "fminnum";
        case Op::FMaxNum: return "fmaxnum";
        case Op::FMinimum: return "fminimum";
        case Op::FMaximum: return "fmaximum";
        case Op::FFloor: return "ffloor";
        case Op::FCeil: return "fceil";
        case Op::FTruncI: return "ftrunc";
        case Op::FRoundA: return "fround";
        case Op::FRoundE: return "froundeven";
        case Op::FOeq: return "fcmp.oeq";
        case Op::FOlt: return "fcmp.olt";
        case Op::FOle: return "fcmp.ole";
        case Op::FUno: return "fcmp.uno";
        case Op::FToSI: return "fptosi";
        case Op::FToUI: return "fptoui";
        case Op::SIToF: return "sitofp";
        case Op::UIToF: return "uitofp";
        case Op::FConv: return "fpconv";
        case Op::FToSIOvf: return "fptosi.ovf";
        case Op::FToUIOvf: return "fptoui.ovf";
        case Op::FIsNaN: return "fisnan";
        case Op::FIsZero: return "fiszero";
        case Op::FIsInf: return "fisinf";
        case Op::FLibm: return "libm";
    }
    return "?";
}

namespace {

std::string arg_text(const Function& fn, const Arg& a) {
    if (a.is_const) return "i" + std::to_string(a.width) + " " + std::to_string(a.bits);
    if (a.var < 0 || a.var >= static_cast<int>(fn.vars.size())) return "%?";
    return "%" + fn.vars[static_cast<std::size_t>(a.var)].name;
}

std::string var_text(const Function& fn, int v) {
    if (v < 0 || v >= static_cast<int>(fn.vars.size())) return "%?";
    auto& x = fn.vars[static_cast<std::size_t>(v)];
    return "%" + x.name + ":i" + std::to_string(x.width);
}

std::string mem_stmt_text(const Function& fn, const Stmt& s) {
    std::string args;
    for (std::size_t k = 0; k < s.args.size(); ++k) args += (k ? ", " : "") + arg_text(fn, s.args[k]);
    switch (s.kind) {
        case Stmt::Alloc:
            return var_text(fn, s.dst) + " = alloc " + mem_kind_name(s.mkind) + " " + args + " align " +
                   std::to_string(s.align) + " init " + std::to_string(s.init) + (s.msg.empty() ? "" : "  ; " + s.msg);
        case Stmt::Free: return "free " + args;
        case Stmt::Load:
            return var_text(fn, s.dst) + ", " + var_text(fn, s.dst2) + ", " + var_text(fn, s.dst3) + " = load " + args +
                   (s.tag ? " tag " + std::to_string(s.tag) : "");
        case Stmt::Store: return "store " + args + (s.tag ? " tag " + std::to_string(s.tag) : "");
        case Stmt::MemCpy: return "memcpy " + args;
        case Stmt::MemSet: return "memset " + args;
        case Stmt::StackSave: return var_text(fn, s.dst) + " = stacksave";
        case Stmt::StackRestore: return "stackrestore " + args;
        case Stmt::Revive: return "revive " + args;
        default: return "?";
    }
}

}  // namespace

std::string to_text(const Function& fn) {
    std::ostringstream o;
    o << "fn @" << fn.ir_name << "(";
    for (std::size_t i = 0; i < fn.params.size(); ++i) {
        auto& v = fn.vars[static_cast<std::size_t>(fn.params[i])];
        o << (i ? ", " : "") << "i" << v.width << " %" << v.name;
    }
    o << ") -> " << (fn.ret_width ? "i" + std::to_string(fn.ret_width) : std::string("void"))
      << " {\n";
    for (std::size_t b = 0; b < fn.blocks.size(); ++b) {
        auto& bl = fn.blocks[b];
        o << "bb" << b << " " << bl.name << ":\n";
        for (auto& p : bl.phis) {
            auto& v = fn.vars[static_cast<std::size_t>(p.dst)];
            o << "  %" << v.name << " = phi i" << v.width;
            for (auto& [pred, a] : p.in) o << " [bb" << pred << ": " << arg_text(fn, a) << "]";
            o << "\n";
        }
        for (auto& s : bl.stmts) {
            switch (s.kind) {
                case Stmt::Assign: {
                    auto& v = fn.vars[static_cast<std::size_t>(s.dst)];
                    o << "  %" << v.name << " = " << op_name(s.op) << " i" << v.width;
                    for (std::size_t k = 0; k < s.args.size(); ++k)
                        o << (k ? ", " : " ") << arg_text(fn, s.args[k]);
                    if (s.uninit) o << "  ; uninit";
                    if (s.nondet) o << "  ; nondet";
                    if (s.op == Op::FLibm) o << "  ; " << s.msg;
                    o << "\n";
                    break;
                }
                case Stmt::Check:
                    o << "  check " << arg_text(fn, s.args[0]) << "  ; " << s.prop << " " << s.cls
                      << " line " << s.line << "\n";
                    break;
                case Stmt::Assume:
                    o << "  assume " << arg_text(fn, s.args[0]) << "\n";
                    break;
                default: o << "  " << mem_stmt_text(fn, s) << "\n"; break;
            }
        }
        auto& t = bl.term;
        switch (t.kind) {
            case Term::Jmp: o << "  jmp bb" << t.t << "\n"; break;
            case Term::Br:
                o << "  br " << arg_text(fn, t.cond) << ", bb" << t.t << ", bb" << t.f << "\n";
                break;
            case Term::Ret:
                o << "  ret" << (t.val ? " " + arg_text(fn, *t.val) : std::string()) << "\n";
                break;
            case Term::Stop: o << "  stop\n"; break;
        }
    }
    o << "}\n";
    return o.str();
}

// ---------------------------------------------------------------------------
// SHA-256 (FIPS 180-4)
// ---------------------------------------------------------------------------
std::string sha256_hex(std::string_view data) {
    static const std::array<uint32_t, 64> K = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4,
        0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe,
        0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f,
        0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
        0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc,
        0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
        0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116,
        0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
        0xc67178f2};
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                     0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    std::string msg(data);
    uint64_t bitlen = static_cast<uint64_t>(msg.size()) * 8;
    msg.push_back(static_cast<char>(0x80));
    while (msg.size() % 64 != 56) msg.push_back('\0');
    for (int i = 7; i >= 0; --i) msg.push_back(static_cast<char>((bitlen >> (i * 8)) & 0xff));
    auto rotr = [](uint32_t x, int n) { return (x >> n) | (x << (32 - n)); };
    for (std::size_t off = 0; off < msg.size(); off += 64) {
        uint32_t w[64];
        for (int i = 0; i < 16; ++i) {
            auto b = [&](int k) {
                return static_cast<uint32_t>(static_cast<unsigned char>(msg[off + static_cast<std::size_t>(i * 4 + k)]));
            };
            w[i] = (b(0) << 24) | (b(1) << 16) | (b(2) << 8) | b(3);
        }
        for (int i = 16; i < 64; ++i) {
            uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; ++i) {
            uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
            uint32_t ch = (e & f) ^ (~e & g);
            uint32_t t1 = hh + S1 + ch + K[static_cast<std::size_t>(i)] + w[i];
            uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
            uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
            uint32_t t2 = S0 + mj;
            hh = g;
            g = f;
            f = e;
            e = d + t1;
            d = c;
            c = b;
            b = a;
            a = t1 + t2;
        }
        h[0] += a;
        h[1] += b;
        h[2] += c;
        h[3] += d;
        h[4] += e;
        h[5] += f;
        h[6] += g;
        h[7] += hh;
    }
    char buf[65];
    for (int i = 0; i < 8; ++i) std::snprintf(buf + i * 8, 9, "%08x", h[i]);
    return std::string(buf, 64);
}

// ---------------------------------------------------------------------------
// Interpreter
// ---------------------------------------------------------------------------
namespace {

uint64_t mask(unsigned w) { return w >= 64 ? ~uint64_t{0} : ((uint64_t{1} << w) - 1); }

int64_t sval(uint64_t v, unsigned w) {
    if (w >= 64) return static_cast<int64_t>(v);
    uint64_t sign = uint64_t{1} << (w - 1);
    v &= mask(w);
    return static_cast<int64_t>((v ^ sign) - sign);
}

uint64_t smin(unsigned w) { return (uint64_t{1} << (w - 1)) & mask(w); }

}  // namespace

uint64_t eval_op(Op op, unsigned w, const std::vector<uint64_t>& a, const std::vector<unsigned>& aw) {
    auto m = mask(w);
    auto x = a.empty() ? 0 : a[0];
    auto y = a.size() > 1 ? a[1] : 0;
    unsigned xw = aw.empty() ? w : aw[0];
    switch (op) {
        case Op::Copy: return x & m;
        case Op::Havoc: return 0;
        case Op::Add: return (x + y) & m;
        case Op::Sub: return (x - y) & m;
        case Op::Mul: return (x * y) & m;
        case Op::UDiv: return y == 0 ? m : (x / y) & m;  // Z3 bvudiv semantics
        case Op::URem: return y == 0 ? x : (x % y) & m;  // Z3 bvurem semantics
        case Op::SDiv: {
            if (y == 0) return sval(x, w) < 0 ? 1 : m;  // Z3 bvsdiv x 0
            if (x == smin(w) && y == m) return x;
            return static_cast<uint64_t>(sval(x, w) / sval(y, w)) & m;
        }
        case Op::SRem: {
            if (y == 0) return x;
            if (x == smin(w) && y == m) return 0;
            return static_cast<uint64_t>(sval(x, w) % sval(y, w)) & m;
        }
        case Op::Shl: return y >= w ? 0 : (x << y) & m;
        case Op::LShr: return y >= w ? 0 : (x >> y) & m;
        case Op::AShr: {
            auto sx = sval(x, w);
            if (y >= w) return sx < 0 ? m : 0;
            return static_cast<uint64_t>(sx >> y) & m;
        }
        case Op::And: return x & y;
        case Op::Or: return x | y;
        case Op::Xor: return (x ^ y) & m;
        case Op::Eq: return x == y;
        case Op::Ne: return x != y;
        case Op::Ult: return x < y;
        case Op::Ule: return x <= y;
        case Op::Ugt: return x > y;
        case Op::Uge: return x >= y;
        case Op::Slt: return sval(x, xw) < sval(y, xw);
        case Op::Sle: return sval(x, xw) <= sval(y, xw);
        case Op::Sgt: return sval(x, xw) > sval(y, xw);
        case Op::Sge: return sval(x, xw) >= sval(y, xw);
        case Op::Select: return (x & 1) ? (a[1] & m) : (a[2] & m);
        case Op::ZExt: return x & mask(xw) & m;
        case Op::SExt: return static_cast<uint64_t>(sval(x, xw)) & m;
        case Op::Trunc: return x & m;
        case Op::SMax: return sval(x, w) >= sval(y, w) ? x : y;
        case Op::SMin: return sval(x, w) <= sval(y, w) ? x : y;
        case Op::UMax: return x >= y ? x : y;
        case Op::UMin: return x <= y ? x : y;
        case Op::Abs: return sval(x, w) < 0 ? (~x + 1) & m : x;
        case Op::Ctlz: {
            uint64_t n = 0;
            for (int i = static_cast<int>(w) - 1; i >= 0 && !((x >> i) & 1); --i) ++n;
            return n & m;
        }
        case Op::Cttz: {
            uint64_t n = 0;
            for (unsigned i = 0; i < w && !((x >> i) & 1); ++i) ++n;
            return n & m;
        }
        case Op::Ctpop: {
            uint64_t n = 0;
            for (unsigned i = 0; i < w; ++i) n += (x >> i) & 1;
            return n & m;
        }
        case Op::Bswap: {
            uint64_t r = 0;
            for (unsigned i = 0; i < w / 8; ++i) r |= ((x >> (i * 8)) & 0xff) << ((w / 8 - 1 - i) * 8);
            return r & m;
        }
        case Op::SAddOvf: {
            auto r = sval((x + y) & mask(xw), xw);
            __int128 t = static_cast<__int128>(sval(x, xw)) + sval(y, xw);
            return t != r;
        }
        case Op::SSubOvf: {
            auto r = sval((x - y) & mask(xw), xw);
            __int128 t = static_cast<__int128>(sval(x, xw)) - sval(y, xw);
            return t != r;
        }
        case Op::SMulOvf: {
            auto r = sval((x * y) & mask(xw), xw);
            __int128 t = static_cast<__int128>(sval(x, xw)) * sval(y, xw);
            return t != r;
        }
        case Op::UAddOvf: return ((x + y) & mask(xw)) < x || (xw == 64 && x + y < x);
        case Op::USubOvf: return y > x;
        case Op::UMulOvf: {
            unsigned __int128 t = static_cast<unsigned __int128>(x) * y;
            return t > mask(xw);
        }
        case Op::SDivOvf: return x == smin(xw) && y == mask(xw);
        case Op::ShiftOob: return y >= xw;
        case Op::ShlSOvf: {
            if (y >= xw) return 1;
            if (sval(x, xw) < 0) return 1;
            // a * 2^b must fit in the signed range: top b+1 bits of a are 0
            return (x >> (xw - 1 - y)) != 0;
        }
        case Op::ShlNswOvf: {
            if (y >= xw) return 1;
            auto r = (x << y) & mask(xw);
            return (static_cast<uint64_t>(sval(r, xw) >> y) & mask(xw)) != x;
        }
        case Op::ShlNuwOvf: {
            if (y >= xw) return 1;
            auto r = (x << y) & mask(xw);
            return (r >> y) != x;
        }
        case Op::LostBitsL: {
            if (y >= xw) return 1;
            return ((x >> y) << y & mask(xw)) != x;
        }
        case Op::LostBitsA: {
            if (y >= xw) return 1;
            auto r = static_cast<uint64_t>(sval(x, xw) >> y) & mask(xw);
            return ((r << y) & mask(xw)) != x;
        }
        case Op::InexactU: return y != 0 && (x % y) != 0;
        case Op::InexactS: {
            if (y == 0) return 0;
            if (x == smin(xw) && y == mask(xw)) return 0;
            return (sval(x, xw) % sval(y, xw)) != 0;
        }
        case Op::ObjSize:
        case Op::ObjLive:
        case Op::ObjKind:
        case Op::ObjAlign: return 0;  // memory queries: evaluated by interpret() on its memory
        default: break;
    }
    if (fp::is_fp_op(op)) return fp::eval(op, w, a, aw) & m;
    return 0;
}

InterpResult interpret(const Function& fn, const std::vector<uint64_t>& args, uint64_t step_limit) {
    InterpResult r;
    mem::ConcMem M;
    // a store/copy/set whose address or length depends on a havoc makes every
    // later load depend on it (the concrete run picked one of many writes)
    bool mem_taint = false;
    std::vector<uint64_t> val(fn.vars.size(), 0);
    std::vector<char> taint(fn.vars.size(), 0);
    for (std::size_t i = 0; i < fn.params.size() && i < args.size(); ++i) {
        auto v = static_cast<std::size_t>(fn.params[i]);
        val[v] = args[i] & mask(fn.vars[v].width);
    }
    auto get = [&](const Arg& a) -> uint64_t {
        return a.is_const ? a.bits : val[static_cast<std::size_t>(a.var)];
    };
    auto tainted = [&](const Arg& a) -> bool {
        return !a.is_const && taint[static_cast<std::size_t>(a.var)];
    };
    int prev = -1, cur = 0;
    uint64_t steps = 0;
    while (true) {
        if (cur < 0 || cur >= static_cast<int>(fn.blocks.size())) {
            r.status = InterpResult::Stopped;
            return r;
        }
        auto& bl = fn.blocks[static_cast<std::size_t>(cur)];
        // phis: parallel assignment from the predecessor
        std::vector<std::pair<std::size_t, std::pair<uint64_t, char>>> upd;
        for (auto& p : bl.phis) {
            for (auto& [pred, a] : p.in) {
                if (pred == prev) {
                    upd.push_back({static_cast<std::size_t>(p.dst), {get(a), static_cast<char>(tainted(a))}});
                    break;
                }
            }
        }
        for (auto& [d, vt] : upd) {
            val[d] = vt.first;
            taint[d] = vt.second;
        }
        for (auto& s : bl.stmts) {
            if (++steps > step_limit) {
                r.status = InterpResult::StepLimit;
                return r;
            }
            if (s.kind == Stmt::Check) {
                if (get(s.args[0]) & 1) {
                    r.status = InterpResult::Violation;
                    r.prop = s.prop;
                    r.cls = s.cls;
                    r.line = s.line;
                    return r;
                }
                continue;
            }
            if (s.kind == Stmt::Assume) {
                if (!(get(s.args[0]) & 1)) {
                    r.status = InterpResult::AssumeFailed;
                    return r;
                }
                continue;
            }
            if (s.kind != Stmt::Assign) {
                // memory statements (memory.hpp ConcMem; same semantics as SymMem)
                auto A = [&](std::size_t i) { return get(s.args[i]); };
                auto setv = [&](int dst, uint64_t v, bool t) {
                    if (dst < 0) return;
                    val[static_cast<std::size_t>(dst)] = v & mask(fn.vars[static_cast<std::size_t>(dst)].width);
                    taint[static_cast<std::size_t>(dst)] = t;
                };
                switch (s.kind) {
                    case Stmt::Alloc: {
                        auto p = M.alloc(A(0), s.mkind, s.align, s.init);
                        if (!p) {
                            r.status = InterpResult::StepLimit;  // too large to run concretely
                            return r;
                        }
                        setv(s.dst, *p, tainted(s.args[0]));
                        break;
                    }
                    case Stmt::Free: M.free(A(0)); break;
                    case Stmt::Revive: M.revive(A(0)); break;
                    case Stmt::Load: {
                        unsigned w = fn.vars[static_cast<std::size_t>(s.dst)].width;
                        unsigned n = (w + 7) / 8;
                        uint64_t v = 0, umask = 0;
                        bool uninit = false, t = tainted(s.args[0]) || mem_taint, tagbad = false;
                        for (unsigned k = 0; k < n; ++k) {
                            auto c = M.read(A(0) + k);
                            v |= static_cast<uint64_t>(c.val) << (8 * k);
                            if (!c.init) umask |= uint64_t{1} << k;
                            uninit = uninit || !c.init;
                            t = t || c.taint;
                            if (M.tags && s.tag && c.tag && c.tag != s.tag) tagbad = true;
                        }
                        setv(s.dst, v, t);
                        if (s.dst2 >= 0)
                            setv(s.dst2, fn.vars[static_cast<std::size_t>(s.dst2)].width == 1 ? uint64_t{uninit} : umask,
                                 false);
                        setv(s.dst3, tagbad, false);
                        break;
                    }
                    case Stmt::Store: {
                        unsigned n = (s.args[1].width + 7) / 8;
                        uint64_t v = A(1);
                        bool t = tainted(s.args[1]);
                        if (tainted(s.args[0])) mem_taint = true;
                        const bool per_byte = s.args[2].width > 1;
                        for (unsigned k = 0; k < n; ++k)
                            M.write(A(0) + k, mem::ConcMem::Cell{static_cast<uint8_t>(v >> (8 * k)),
                                                                 ((A(2) >> (per_byte ? k : 0)) & 1) != 0,
                                                                 static_cast<uint8_t>(s.tag), t});
                        break;
                    }
                    case Stmt::MemCpy: {
                        uint64_t n = A(2);
                        if (tainted(s.args[0]) || tainted(s.args[2])) mem_taint = true;
                        if (n > (uint64_t{1} << 24)) {
                            r.status = InterpResult::StepLimit;
                            return r;
                        }
                        std::vector<mem::ConcMem::Cell> tmp;
                        for (uint64_t k = 0; k < n; ++k) tmp.push_back(M.read(A(1) + k));
                        for (uint64_t k = 0; k < n; ++k) M.write(A(0) + k, tmp[static_cast<std::size_t>(k)]);
                        steps += n;
                        break;
                    }
                    case Stmt::MemSet: {
                        uint64_t n = A(2);
                        if (tainted(s.args[0]) || tainted(s.args[2])) mem_taint = true;
                        if (n > (uint64_t{1} << 24)) {
                            r.status = InterpResult::StepLimit;
                            return r;
                        }
                        for (uint64_t k = 0; k < n; ++k)
                            M.write(A(0) + k, mem::ConcMem::Cell{static_cast<uint8_t>(A(1)), true, 0, tainted(s.args[1])});
                        steps += n;
                        break;
                    }
                    case Stmt::StackSave: setv(s.dst, M.token(), false); break;
                    case Stmt::StackRestore: M.stack_restore(A(0)); break;
                    default: break;
                }
                continue;
            }
            auto d = static_cast<std::size_t>(s.dst);
            unsigned w = fn.vars[d].width;
            if (s.op == Op::ObjSize || s.op == Op::ObjLive || s.op == Op::ObjKind || s.op == Op::ObjAlign) {
                auto p = get(s.args[0]);
                uint64_t v = s.op == Op::ObjSize ? M.size(p) : s.op == Op::ObjLive ? M.live(p)
                             : s.op == Op::ObjKind ? M.kind(p) : M.align(p);
                val[d] = v & mask(w);
                taint[d] = tainted(s.args[0]);
                continue;
            }
            if (s.op == Op::Havoc) {
                val[d] = 0;
                taint[d] = 1;
                continue;
            }
            std::vector<uint64_t> xs;
            std::vector<unsigned> ws;
            bool t = false;
            for (auto& a : s.args) {
                xs.push_back(get(a));
                ws.push_back(a.width);
                t = t || tainted(a);
            }
            if (s.op == Op::FLibm) {
                // unmodelled libm call: the host library's value
                val[d] = fp::eval(s.op, w, xs, ws, s.msg) & mask(w);
                taint[d] = t;
                continue;
            }
            if (fp::is_fp_op(s.op) && fp::ambiguous(s.op, w, xs, ws)) {
                // fmuladd fused or not, minnum(+0, -0): the compiler's choice; a
                // result that depends on it is not replayable
                val[d] = fp::eval(s.op, w, xs, ws) & mask(w);
                taint[d] = true;
                continue;
            }
            val[d] = eval_op(s.op, w, xs, ws) & mask(w);
            taint[d] = t;
        }
        auto& tm = bl.term;
        prev = cur;
        switch (tm.kind) {
            case Term::Jmp: cur = tm.t; break;
            case Term::Br:
                if (tainted(tm.cond)) r.ret_nondet = true;
                cur = (get(tm.cond) & 1) ? tm.t : tm.f;
                break;
            case Term::Ret:
                r.status = InterpResult::Returned;
                if (tm.val) {
                    r.ret = get(*tm.val);
                    r.ret_nondet = r.ret_nondet || tainted(*tm.val);
                }
                return r;
            case Term::Stop:
                r.status = InterpResult::Stopped;
                return r;
        }
        if (++steps > step_limit) {
            r.status = InterpResult::StepLimit;
            return r;
        }
    }
}

}  // namespace prism::pir
