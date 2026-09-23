// IEEE 754 binary16/32/64 concrete semantics for PIR (docs/PIR.md "Floating
// point"). float and double use the host's SSE arithmetic (round to nearest
// even, no excess precision on x86-64); half is computed in double and
// rounded once to binary16, which is exact for + - * / sqrt because
// 53 >= 2 * 11 + 2 (double rounding is innocuous). fma/fmuladd on half is
// refused by the translator, so it never reaches this file.

#include "fp.hpp"

#include <cfenv>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>

namespace prism::pir::fp {

namespace {

uint64_t mask(unsigned w) { return w >= 64 ? ~uint64_t{0} : ((uint64_t{1} << w) - 1); }

double half_to_double(uint16_t h) {
    unsigned sign = h >> 15, e = (h >> 10) & 0x1F, m = h & 0x3FF;
    double v;
    if (e == 0) v = std::ldexp(static_cast<double>(m), -24);
    else if (e == 31) v = m ? std::nan("") : INFINITY;
    else v = std::ldexp(static_cast<double>(m | 0x400), static_cast<int>(e) - 25);
    return sign ? -v : v;
}

uint16_t double_to_half(double d) {
    uint16_t sign = std::signbit(d) ? 0x8000 : 0;
    if (std::isnan(d)) return static_cast<uint16_t>(sign | 0x7E00);
    double a = std::fabs(d);
    if (a >= 65520.0) return static_cast<uint16_t>(sign | 0x7C00);  // rounds to infinity (65504 is odd)
    if (a < std::ldexp(1.0, -14)) {
        // subnormal (or zero): units of 2^-24; exact scaling, one rounding
        auto r = static_cast<uint16_t>(std::nearbyint(std::ldexp(a, 24)));
        return static_cast<uint16_t>(sign | r);  // r == 1024 is the smallest normal
    }
    int e = 0;
    std::frexp(a, &e);  // a = m * 2^e, m in [0.5, 1): half exponent e - 1
    double q = std::nearbyint(std::ldexp(a, 11 - e));  // in [1024, 2048]
    int he = e - 1;
    if (q >= 2048.0) {
        q = 1024.0;
        ++he;
    }
    if (he > 15) return static_cast<uint16_t>(sign | 0x7C00);
    return static_cast<uint16_t>(sign | ((he + 15) << 10) | (static_cast<unsigned>(q) - 1024));
}

float as_float(uint64_t b) {
    auto u = static_cast<uint32_t>(b);
    float f;
    std::memcpy(&f, &u, 4);
    return f;
}

uint64_t float_bits(float f) {
    uint32_t u;
    std::memcpy(&u, &f, 4);
    return u;
}

uint64_t double_bits(double d) {
    uint64_t u;
    std::memcpy(&u, &d, 8);
    return u;
}

int64_t sext(uint64_t v, unsigned w) {
    if (w >= 64) return static_cast<int64_t>(v);
    uint64_t sign = uint64_t{1} << (w - 1);
    v &= mask(w);
    return static_cast<int64_t>((v ^ sign) - sign);
}

uint64_t quiet_nan(unsigned w) { return w == 16 ? 0x7E00 : w == 32 ? 0x7FC00000 : 0x7FF8000000000000; }

using D1 = double (*)(double);
using F1 = float (*)(float);
using D2 = double (*)(double, double);
using F2 = float (*)(float, float);

struct Libm {
    D1 d1 = nullptr;
    F1 f1 = nullptr;
    D2 d2 = nullptr;
    F2 f2 = nullptr;
};

const std::map<std::string, Libm>& libm_table() {
    static const std::map<std::string, Libm> k = [] {
        std::map<std::string, Libm> t;
        auto one = [&](const char* n, D1 d, F1 f) {
            t[n].d1 = d;
            t[std::string(n) + "f"].f1 = f;
        };
        auto two = [&](const char* n, D2 d, F2 f) {
            t[n].d2 = d;
            t[std::string(n) + "f"].f2 = f;
        };
        one("sin", ::sin, ::sinf);
        one("cos", ::cos, ::cosf);
        one("tan", ::tan, ::tanf);
        one("asin", ::asin, ::asinf);
        one("acos", ::acos, ::acosf);
        one("atan", ::atan, ::atanf);
        one("sinh", ::sinh, ::sinhf);
        one("cosh", ::cosh, ::coshf);
        one("tanh", ::tanh, ::tanhf);
        one("asinh", ::asinh, ::asinhf);
        one("acosh", ::acosh, ::acoshf);
        one("atanh", ::atanh, ::atanhf);
        one("exp", ::exp, ::expf);
        one("exp2", ::exp2, ::exp2f);
        one("expm1", ::expm1, ::expm1f);
        one("log", ::log, ::logf);
        one("log2", ::log2, ::log2f);
        one("log10", ::log10, ::log10f);
        one("log1p", ::log1p, ::log1pf);
        one("cbrt", ::cbrt, ::cbrtf);
        one("erf", ::erf, ::erff);
        one("erfc", ::erfc, ::erfcf);
        one("tgamma", ::tgamma, ::tgammaf);
        two("atan2", ::atan2, ::atan2f);
        two("pow", ::pow, ::powf);
        two("hypot", ::hypot, ::hypotf);
        return t;
    }();
    return k;
}

}  // namespace

std::optional<unsigned> width_of(const ir::Type& t) {
    if (t.kind != ir::Type::Float) return std::nullopt;
    if (t.text == "half") return 16;
    if (t.text == "float") return 32;
    if (t.text == "double") return 64;
    return std::nullopt;
}

bool is_fp_op(Op op) { return static_cast<int>(op) >= static_cast<int>(Op::FAdd); }

bool is_nan(uint64_t bits, unsigned w) {
    switch (w) {
        case 16: return ((bits >> 10) & 0x1F) == 0x1F && (bits & 0x3FF);
        case 32: return ((bits >> 23) & 0xFF) == 0xFF && (bits & 0x7FFFFF);
        case 64: return ((bits >> 52) & 0x7FF) == 0x7FF && (bits & 0xFFFFFFFFFFFFF);
        default: return false;
    }
}

double to_double(uint64_t bits, unsigned w) {
    if (w == 16) return half_to_double(static_cast<uint16_t>(bits));
    if (w == 32) return static_cast<double>(as_float(bits));
    double d;
    std::memcpy(&d, &bits, 8);
    return d;
}

uint64_t from_double(double d, unsigned w) {
    if (w == 16) return double_to_half(d);
    if (w == 32) return float_bits(static_cast<float>(d));
    return double_bits(d);
}

std::optional<uint64_t> from_double_bits(uint64_t dbits, unsigned w) {
    if (w == 64) return dbits;
    double d;
    std::memcpy(&d, &dbits, 8);
    if (std::isnan(d)) {
        // LLVM writes a float/half NaN as the double with the same payload
        // in the top mantissa bits
        uint64_t sign = dbits >> 63, pay = dbits & 0xFFFFFFFFFFFFF;
        if (w == 32) return (sign << 31) | 0x7F800000 | (pay >> 29);
        return (sign << 15) | 0x7C00 | (pay >> 42);
    }
    auto b = from_double(d, w);
    if (double_bits(to_double(b, w)) != dbits) return std::nullopt;  // not exact in the format
    return b;
}

bool is_libm_value(const std::string& name) { return libm_table().count(name) > 0; }

uint64_t eval(Op op, unsigned w, const std::vector<uint64_t>& a, const std::vector<unsigned>& aw,
              const std::string& libm, bool fused) {
    auto W = [&](std::size_t i) { return i < aw.size() ? aw[i] : w; };
    auto D = [&](std::size_t i) { return to_double(a.at(i), W(i)); };
    auto nan = [&](std::size_t i) { return std::isnan(D(i)); };
    auto res = [&](double d) { return from_double(d, w) & mask(w); };
    // binary op in the result format (float: in float; half: double, then rounded)
    auto arith = [&](char k) -> uint64_t {
        if (w == 32) {
            float x = as_float(a.at(0)), y = as_float(a.at(1)), r = 0;
            switch (k) {
                case '+': r = x + y; break;
                case '-': r = x - y; break;
                case '*': r = x * y; break;
                case '/': r = x / y; break;
                default: r = std::fmod(x, y); break;
            }
            return float_bits(r);
        }
        double x = D(0), y = D(1), r = 0;
        switch (k) {
            case '+': r = x + y; break;
            case '-': r = x - y; break;
            case '*': r = x * y; break;
            case '/': r = x / y; break;
            default: r = std::fmod(x, y); break;
        }
        return res(r);
    };
    switch (op) {
        case Op::FAdd: return arith('+');
        case Op::FSub: return arith('-');
        case Op::FMul: return arith('*');
        case Op::FDiv: return arith('/');
        case Op::FRem: return arith('%');
        case Op::FSqrt:
            if (w == 32) return float_bits(std::sqrt(as_float(a.at(0))));
            return res(std::sqrt(D(0)));
        case Op::FFma:
        case Op::FMulAdd: {
            bool f = op == Op::FFma || fused;
            if (w == 32) {
                float x = as_float(a.at(0)), y = as_float(a.at(1)), z = as_float(a.at(2));
                if (f) return float_bits(std::fma(x, y, z));
                volatile float m = x * y;  // rounded product, then the addition
                return float_bits(m + z);
            }
            if (f) return res(std::fma(D(0), D(1), D(2)));
            volatile double m = D(0) * D(1);
            return res(m + D(2));
        }
        case Op::FMinNum:
        case Op::FMaxNum: {
            if (nan(0)) return a.at(1) & mask(w);
            if (nan(1)) return a.at(0) & mask(w);
            double x = D(0), y = D(1);
            bool mn = op == Op::FMinNum;
            if (mn ? x < y : y < x) return a.at(0) & mask(w);
            if (mn ? y < x : x < y) return a.at(1) & mask(w);
            return a.at(0) & mask(w);  // equal (maybe +0 / -0: see ambiguous())
        }
        case Op::FMinimum:
        case Op::FMaximum: {
            if (nan(0) || nan(1)) return quiet_nan(w);
            double x = D(0), y = D(1);
            bool mn = op == Op::FMinimum;
            if (mn ? x < y : y < x) return a.at(0) & mask(w);
            if (mn ? y < x : x < y) return a.at(1) & mask(w);
            bool xneg = std::signbit(x);
            return (mn ? xneg : !xneg) ? a.at(0) & mask(w) : a.at(1) & mask(w);
        }
        case Op::FFloor: return res(std::floor(D(0)));
        case Op::FCeil: return res(std::ceil(D(0)));
        case Op::FTruncI: return res(std::trunc(D(0)));
        case Op::FRoundA: return res(std::round(D(0)));
        case Op::FRoundE: {
            int old = std::fegetround();
            std::fesetround(FE_TONEAREST);
            double r = std::nearbyint(D(0));
            std::fesetround(old);
            return res(r);
        }
        case Op::FOeq: return D(0) == D(1);
        case Op::FOlt: return D(0) < D(1);
        case Op::FOle: return D(0) <= D(1);
        case Op::FUno: return nan(0) || nan(1);
        case Op::FIsNaN: return nan(0);
        case Op::FIsZero: return D(0) == 0.0;
        case Op::FIsInf: return std::isinf(D(0));
        case Op::FToSI:
        case Op::FToUI: {
            double t = std::trunc(D(0));
            if (std::isnan(t)) return 0;
            if (op == Op::FToSI) {
                if (t < -9223372036854775808.0 || t >= 9223372036854775808.0) return 0;
                return static_cast<uint64_t>(static_cast<int64_t>(t)) & mask(w);
            }
            if (t < 0 || t >= 18446744073709551616.0) return 0;
            return static_cast<uint64_t>(t) & mask(w);
        }
        case Op::SIToF: {
            int64_t v = sext(a.at(0), W(0));
            if (w == 32) return float_bits(static_cast<float>(v));
            return res(static_cast<double>(v));
        }
        case Op::UIToF: {
            uint64_t v = a.at(0) & mask(W(0));
            if (w == 32) return float_bits(static_cast<float>(v));
            return res(static_cast<double>(v));
        }
        case Op::FConv:
            if (w == W(0)) return a.at(0) & mask(w);
            return res(D(0));
        case Op::FToSIOvf:
        case Op::FToUIOvf: {
            double x = D(0);
            if (std::isnan(x) || std::isinf(x)) return 1;
            double t = std::trunc(x);
            auto k = static_cast<int>(a.at(1));
            if (op == Op::FToSIOvf) return t < -std::ldexp(1.0, k - 1) || t >= std::ldexp(1.0, k - 1);
            return t < 0 || t >= std::ldexp(1.0, k);
        }
        case Op::FLibm: {
            auto it = libm_table().find(libm);
            if (it == libm_table().end()) return 0;
            const auto& f = it->second;
            if (w == 32) {
                if (f.f1) return float_bits(f.f1(as_float(a.at(0))));
                if (f.f2) return float_bits(f.f2(as_float(a.at(0)), as_float(a.at(1))));
                return 0;
            }
            if (f.d1) return res(f.d1(D(0)));
            if (f.d2) return res(f.d2(D(0), D(1)));
            return 0;
        }
        default: return 0;
    }
}

bool ambiguous(Op op, unsigned w, const std::vector<uint64_t>& a, const std::vector<unsigned>& aw) {
    if (op == Op::FMulAdd) return eval(op, w, a, aw, {}, true) != eval(op, w, a, aw, {}, false);
    if (op == Op::FMinNum || op == Op::FMaxNum) {
        double x = to_double(a.at(0), w), y = to_double(a.at(1), w);
        return !std::isnan(x) && !std::isnan(y) && x == y && (a.at(0) & mask(w)) != (a.at(1) & mask(w));
    }
    return false;
}

std::string format(uint64_t bits, unsigned w) {
    double d = to_double(bits, w);
    if (std::isnan(d)) return "nan";
    if (std::isinf(d)) return d < 0 ? "-inf" : "inf";
    char buf[64];
    std::snprintf(buf, sizeof buf, w == 64 ? "%.17g" : w == 32 ? "%.9g" : "%.5g", d);
    return buf;
}

std::string ll_const(uint64_t bits, unsigned w) {
    char buf[40];
    if (w == 16) {
        std::snprintf(buf, sizeof buf, "0xH%04llX", static_cast<unsigned long long>(bits & 0xFFFF));
        return buf;
    }
    uint64_t d = bits;
    if (w == 32) {
        if (is_nan(bits, 32)) {
            d = ((bits >> 31 & 1) << 63) | 0x7FF0000000000000 | ((bits & 0x7FFFFF) << 29);
        } else {
            d = double_bits(static_cast<double>(as_float(bits)));
        }
    }
    std::snprintf(buf, sizeof buf, "0x%016llX", static_cast<unsigned long long>(d));
    return buf;
}

}  // namespace prism::pir::fp
