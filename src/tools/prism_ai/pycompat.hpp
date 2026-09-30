#pragma once

// Reproducibility helpers for the prism_ai offline tool: the numbers it
// writes into model metrics and docs/AI.md / docs/SOLVERS.md were first
// produced by a Python implementation, so the same seeded sample and the same
// printed digits are kept:
//
//   * PyRandom: the Mersenne Twister of CPython's random.Random(int seed)
//     (init_by_array on the seed's 32-bit words), with randrange(n) and
//     shuffle() drawing exactly as CPython does (getrandbits rejection);
//   * py_round(x, n): round() of a float (exact decimal rounding, half even);
//   * py_repr(x): repr() of a float (shortest round trip; exponent form below
//     1e-4 and from 1e16);
//   * py_fixed / py_signed_fixed / py_percent: the ".1f" / "+.1f" / ".1%" formats.

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

namespace prism_ai {

class PyRandom {
public:
    explicit PyRandom(std::uint64_t seed) {
        std::vector<std::uint32_t> key;
        if (seed == 0) key.push_back(0);
        while (seed) {
            key.push_back(static_cast<std::uint32_t>(seed & 0xffffffffu));
            seed >>= 32;
        }
        init_by_array(key);
    }

    std::uint32_t genrand_uint32() {
        static constexpr std::uint32_t kMag[2] = {0x0u, 0x9908b0dfu};
        if (mti_ >= kN) {
            int kk = 0;
            std::uint32_t y;
            for (; kk < kN - kM; ++kk) {
                y = (mt_[kk] & kUpper) | (mt_[kk + 1] & kLower);
                mt_[kk] = mt_[kk + kM] ^ (y >> 1) ^ kMag[y & 1u];
            }
            for (; kk < kN - 1; ++kk) {
                y = (mt_[kk] & kUpper) | (mt_[kk + 1] & kLower);
                mt_[kk] = mt_[kk + (kM - kN)] ^ (y >> 1) ^ kMag[y & 1u];
            }
            y = (mt_[kN - 1] & kUpper) | (mt_[0] & kLower);
            mt_[kN - 1] = mt_[kM - 1] ^ (y >> 1) ^ kMag[y & 1u];
            mti_ = 0;
        }
        std::uint32_t y = mt_[mti_++];
        y ^= (y >> 11);
        y ^= (y << 7) & 0x9d2c5680u;
        y ^= (y << 15) & 0xefc60000u;
        y ^= (y >> 18);
        return y;
    }

    // random.getrandbits(k) for 0 <= k <= 32.
    std::uint32_t getrandbits(int k) { return k <= 0 ? 0u : genrand_uint32() >> (32 - k); }

    // random.randrange(n) for 0 < n < 2**32 (_randbelow_with_getrandbits).
    std::uint64_t randbelow(std::uint64_t n) {
        int k = 0;
        for (std::uint64_t v = n; v; v >>= 1) ++k;
        std::uint64_t r = getrandbits(k);
        while (r >= n) r = getrandbits(k);
        return r;
    }

    template <class T>
    void shuffle(std::vector<T>& x) {
        for (std::size_t i = x.size(); i-- > 1;) {
            auto j = static_cast<std::size_t>(randbelow(i + 1));
            std::swap(x[i], x[j]);
        }
    }

private:
    static constexpr int kN = 624, kM = 397;
    static constexpr std::uint32_t kUpper = 0x80000000u, kLower = 0x7fffffffu;
    std::array<std::uint32_t, kN> mt_{};
    int mti_ = kN + 1;

    void init_genrand(std::uint32_t s) {
        mt_[0] = s;
        for (mti_ = 1; mti_ < kN; ++mti_)
            mt_[mti_] = 1812433253u * (mt_[mti_ - 1] ^ (mt_[mti_ - 1] >> 30)) + static_cast<std::uint32_t>(mti_);
    }

    void init_by_array(const std::vector<std::uint32_t>& key) {
        init_genrand(19650218u);
        int i = 1;
        std::size_t j = 0;
        const std::size_t len = key.size();
        for (std::size_t k = std::max<std::size_t>(kN, len); k; --k) {
            mt_[i] = (mt_[i] ^ ((mt_[i - 1] ^ (mt_[i - 1] >> 30)) * 1664525u)) + key[j] + static_cast<std::uint32_t>(j);
            ++i;
            ++j;
            if (i >= kN) {
                mt_[0] = mt_[kN - 1];
                i = 1;
            }
            if (j >= len) j = 0;
        }
        for (std::size_t k = kN - 1; k; --k) {
            mt_[i] = (mt_[i] ^ ((mt_[i - 1] ^ (mt_[i - 1] >> 30)) * 1566083941u)) - static_cast<std::uint32_t>(i);
            ++i;
            if (i >= kN) {
                mt_[0] = mt_[kN - 1];
                i = 1;
            }
        }
        mt_[0] = 0x80000000u;
        mti_ = kN;
    }
};

// round(x, n): the double nearest the exactly rounded decimal (the C library
// prints the exact binary value rounded half to even, as CPython does).
inline double py_round(double x, int n) {
    if (!std::isfinite(x)) return x;
    char buf[512];
    std::snprintf(buf, sizeof buf, "%.*f", n, x);
    return std::strtod(buf, nullptr);
}

// repr(float).
inline std::string py_repr(double x) {
    if (std::isnan(x)) return "nan";
    if (std::isinf(x)) return x > 0 ? "inf" : "-inf";
    if (x == 0.0) return std::signbit(x) ? "-0.0" : "0.0";
    char buf[64];
    auto res = std::to_chars(buf, buf + sizeof buf, x, std::chars_format::scientific);
    std::string sci(buf, res.ptr);  // d[.ddd]e[+-]XX
    const bool neg = sci[0] == '-';
    if (neg) sci.erase(0, 1);
    const auto e = sci.find('e');
    const int exp10 = std::atoi(sci.c_str() + e + 1);
    std::string digits;
    for (std::size_t i = 0; i < e; ++i)
        if (sci[i] != '.') digits += sci[i];
    std::string out;
    if (exp10 < -4 || exp10 >= 16) {
        out = digits.substr(0, 1);
        if (digits.size() > 1) out += "." + digits.substr(1);
        char eb[8];
        std::snprintf(eb, sizeof eb, "e%c%02d", exp10 < 0 ? '-' : '+', std::abs(exp10));
        out += eb;
    } else if (exp10 < 0) {
        out = "0." + std::string(static_cast<std::size_t>(-exp10 - 1), '0') + digits;
    } else {
        const auto point = static_cast<std::size_t>(exp10) + 1;
        if (digits.size() <= point) out = digits + std::string(point - digits.size(), '0') + ".0";
        else out = digits.substr(0, point) + "." + digits.substr(point);
    }
    return neg ? "-" + out : out;
}

inline std::string py_fixed(double x, int n) {
    char buf[512];
    std::snprintf(buf, sizeof buf, "%.*f", n, x);
    return buf;
}

inline std::string py_signed_fixed(double x, int n) {
    char buf[512];
    std::snprintf(buf, sizeof buf, "%+.*f", n, x);
    return buf;
}

// f"{x:.1%}"
inline std::string py_percent(double x, int n) { return py_fixed(x * 100.0, n) + "%"; }

}  // namespace prism_ai
