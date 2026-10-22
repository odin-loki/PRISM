#pragma once

// Portable 128-bit integers for MSVC and other targets without __int128.
// Parse/format semantics match SV-COMP witness replay (Python int(text, 0))
// and witness.cpp int_text.

#include <cctype>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#if !defined(PRISM_HAS_INT128) && defined(__SIZEOF_INT128__) && __SIZEOF_INT128__ == 16 && !defined(_MSC_VER)
#  define PRISM_HAS_INT128 1
#endif
#ifndef PRISM_HAS_INT128
#  define PRISM_HAS_INT128 0
#endif

namespace prism {

#if PRISM_HAS_INT128

using i128 = __int128;
using u128 = unsigned __int128;

inline u128 u128_abs(i128 v) {
    return v < 0 ? static_cast<u128>(-(v + 1)) + 1 : static_cast<u128>(v);
}

inline i128 i128_neg(i128 v) { return -v; }

inline bool i128_eq(i128 a, i128 b) { return a == b; }

inline i128 i128_add(i128 a, i128 b) { return a + b; }

inline i128 i128_sub(i128 a, i128 b) { return a - b; }

inline i128 i128_mul(i128 a, i128 b) { return a * b; }

inline i128 i128_max(i128 a, i128 b) { return a > b ? a : b; }

inline i128 i128_min(i128 a, i128 b) { return a < b ? a : b; }

inline std::int64_t i128_to_i64(i128 v) { return static_cast<std::int64_t>(v); }

inline double i128_to_double(i128 v) { return static_cast<double>(v); }

inline i128 i128_from_i64(std::int64_t v) { return static_cast<i128>(v); }

inline i128 i128_from_u64(std::uint64_t v) { return static_cast<i128>(static_cast<u128>(v)); }

inline i128 i128_shl(i128 v, int n) { return n < 0 ? v : v << n; }

inline i128 i128_shr(i128 v, int n) { return n < 0 ? v : v >> n; }

inline i128 i128_and(i128 a, i128 b) { return a & b; }

inline i128 i128_from_u128(u128 v) { return static_cast<i128>(v); }

inline u128 i128_to_u128(i128 v) { return static_cast<u128>(v); }

inline u128 u128_shl(u128 v, unsigned n) { return n >= 128 ? u128(0) : v << n; }

inline u128 u128_shr(u128 v, unsigned n) { return n >= 128 ? u128(0) : v >> n; }

inline u128 u128_mask(unsigned w) { return w >= 128 ? ~u128(0) : u128_shl(u128(1), w) - u128(1); }

inline bool u128_bit(u128 v, unsigned k) { return k < 128 && ((v >> k) & 1); }

inline std::string format_i128(i128 v) {
    if (v == 0) return "0";
    bool neg = v < 0;
    u128 u = u128_abs(v);
    std::string s;
    while (u) {
        s += static_cast<char>('0' + static_cast<int>(u % 10));
        u /= 10;
    }
    if (neg) s += '-';
    return {s.rbegin(), s.rend()};
}

#else  // !PRISM_HAS_INT128

struct u128 {
    std::uint64_t lo = 0;
    std::uint64_t hi = 0;

    constexpr u128() = default;
    constexpr u128(std::uint64_t v) : lo(v), hi(0) {}
    constexpr u128(std::uint64_t h, std::uint64_t l) : lo(l), hi(h) {}

    friend constexpr bool operator==(u128 a, u128 b) { return a.lo == b.lo && a.hi == b.hi; }
    friend constexpr bool operator!=(u128 a, u128 b) { return !(a == b); }
    friend constexpr bool operator<(u128 a, u128 b) {
        return a.hi < b.hi || (a.hi == b.hi && a.lo < b.lo);
    }
    friend constexpr bool operator>(u128 a, u128 b) { return b < a; }
    friend constexpr bool operator<=(u128 a, u128 b) { return !(b < a); }
    friend constexpr bool operator>=(u128 a, u128 b) { return !(a < b); }

    friend constexpr u128 operator~(u128 a) { return {~a.hi, ~a.lo}; }

    friend constexpr u128 operator+(u128 a, u128 b) {
        std::uint64_t lo = a.lo + b.lo;
        std::uint64_t carry = lo < a.lo ? 1 : 0;
        return {a.hi + b.hi + carry, lo};
    }

    friend constexpr u128 operator-(u128 a, u128 b) { return a + ~b + u128(1); }

    friend constexpr u128 operator|(u128 a, u128 b) { return {a.hi | b.hi, a.lo | b.lo}; }

    friend constexpr u128 operator&(u128 a, u128 b) { return {a.hi & b.hi, a.lo & b.lo}; }

    friend constexpr u128 operator^(u128 a, u128 b) { return {a.hi ^ b.hi, a.lo ^ b.lo}; }
};

inline u128 operator<<(u128 v, unsigned n) {
    if (n >= 128) return u128(0);
    if (n >= 64) return u128(v.lo << (n - 64), 0);
    return u128((v.hi << n) | (v.lo >> (64 - n)), v.lo << n);
}

inline u128 operator>>(u128 v, unsigned n) {
    if (n >= 128) return u128(0);
    if (n >= 64) return u128(0, v.hi >> (n - 64));
    return u128(v.hi >> n, (v.lo >> n) | (v.hi << (64 - n)));
}

namespace wide_int_detail {

inline u128 mul_u64(std::uint64_t a, std::uint64_t b) {
    const std::uint64_t a0 = a & 0xffffffffu;
    const std::uint64_t a1 = a >> 32;
    const std::uint64_t b0 = b & 0xffffffffu;
    const std::uint64_t b1 = b >> 32;
    const std::uint64_t p0 = a0 * b0;
    const std::uint64_t p1 = a0 * b1;
    const std::uint64_t p2 = a1 * b0;
    const std::uint64_t p3 = a1 * b1;
    std::uint64_t mid = (p0 >> 32) + (p1 & 0xffffffffu) + (p2 & 0xffffffffu);
    const std::uint64_t hi = p3 + (p1 >> 32) + (p2 >> 32) + (mid >> 32);
    const std::uint64_t lo = (mid << 32) | (p0 & 0xffffffffu);
    return {hi, lo};
}

inline u128 mul_u128_u64(u128 a, std::uint64_t b) {
    u128 p = mul_u64(a.lo, b);
    u128 h = mul_u64(a.hi, b);
    u128 out = u128(p.hi + h.lo, p.lo);
    out.hi += h.hi;
    if (p.hi + h.lo < p.hi) ++out.hi;
    return out;
}

inline std::pair<u128, std::uint64_t> divmod_u128_u64(u128 n, std::uint64_t d) {
    if (n.hi == 0) return {u128(n.lo / d), n.lo % d};
    std::uint64_t rem = 0;
    std::uint64_t q_hi = 0;
    for (int i = 63; i >= 0; --i) {
        rem = (rem << 1) | ((n.hi >> i) & 1);
        q_hi = (q_hi << 1) | (rem >= d ? 1u : 0u);
        if (rem >= d) rem -= d;
    }
    std::uint64_t q_lo = 0;
    for (int i = 63; i >= 0; --i) {
        rem = (rem << 1) | ((n.lo >> i) & 1);
        q_lo = (q_lo << 1) | (rem >= d ? 1u : 0u);
        if (rem >= d) rem -= d;
    }
    return {u128(q_hi, q_lo), rem};
}

inline std::pair<u128, u128> divmod_u128_u128(u128 n, u128 d) {
    if (d == u128(0)) return {u128(0), n};
    if (n < d) return {u128(0), n};
    if (d.hi == 0) {
        auto qr = divmod_u128_u64(n, d.lo);
        return {qr.first, u128(qr.second)};
    }
    u128 q;
    u128 r;
    for (int i = 127; i >= 0; --i) {
        const bool bit = i >= 64 ? ((n.hi >> (i - 64)) & 1) != 0 : ((n.lo >> i) & 1) != 0;
        r = (r << 1) | u128(bit ? 1 : 0);
        if (r >= d) {
            r = r - d;
            q = q | (u128(1) << i);
        }
    }
    return {q, r};
}

}  // namespace wide_int_detail

inline u128 operator*(u128 a, u128 b) {
    u128 p0 = wide_int_detail::mul_u64(a.lo, b.lo);
    u128 p1 = wide_int_detail::mul_u64(a.lo, b.hi);
    u128 p2 = wide_int_detail::mul_u64(a.hi, b.lo);
    return p0 + u128(p1.lo, 0) + u128(p2.lo, 0);
}

inline u128 operator/(u128 a, std::uint64_t d) { return wide_int_detail::divmod_u128_u64(a, d).first; }

inline std::uint64_t operator%(u128 a, std::uint64_t d) { return wide_int_detail::divmod_u128_u64(a, d).second; }

inline u128 operator/(u128 a, u128 b) { return wide_int_detail::divmod_u128_u128(a, b).first; }

inline u128 operator%(u128 a, u128 b) { return wide_int_detail::divmod_u128_u128(a, b).second; }

struct i128 {
    u128 bits{};

    constexpr i128() = default;
    constexpr explicit i128(u128 b) : bits(b) {}
    constexpr i128(std::int64_t v) {
        if (v >= 0) bits = u128(static_cast<std::uint64_t>(v));
        else bits = ~u128(static_cast<std::uint64_t>(-v)) + u128(1);
    }

    friend constexpr bool operator==(i128 a, i128 b) { return a.bits == b.bits; }
    friend constexpr bool operator!=(i128 a, i128 b) { return !(a == b); }

    friend constexpr bool operator<(i128 a, i128 b) {
        const bool an = a.bits.hi >> 63;
        const bool bn = b.bits.hi >> 63;
        if (an != bn) return an;
        return a.bits < b.bits;
    }
    friend constexpr bool operator>(i128 a, i128 b) { return b < a; }
    friend constexpr bool operator<=(i128 a, i128 b) { return !(b < a); }
    friend constexpr bool operator>=(i128 a, i128 b) { return !(a < b); }

    friend constexpr i128 operator-(i128 v) { return i128(~v.bits + u128(1)); }

    friend constexpr i128 operator+(i128 a, i128 b) { return i128(a.bits + b.bits); }

    friend constexpr i128 operator-(i128 a, i128 b) { return a + (-b); }

    friend i128 operator*(i128 a, i128 b) {
        const bool neg = (a.bits.hi >> 63) != (b.bits.hi >> 63);
        u128 ua = u128_abs(a);
        u128 ub = u128_abs(b);
        u128 p = ua * ub;
        return neg ? -i128(p) : i128(p);
    }

    explicit operator double() const {
        double h = static_cast<double>(bits.hi);
        double l = static_cast<double>(bits.lo);
        if (bits.hi >> 63) {
            u128 m = u128_abs(*this);
            return -(static_cast<double>(m.hi) * 18446744073709551616.0 + static_cast<double>(m.lo));
        }
        return h * 18446744073709551616.0 + l;
    }
};

inline u128 u128_abs(i128 v) {
    if ((v.bits.hi >> 63) == 0) return v.bits;
    return ~v.bits + u128(1);
}

inline i128 i128_neg(i128 v) { return -v; }

inline bool i128_eq(i128 a, i128 b) { return a == b; }

inline i128 i128_add(i128 a, i128 b) { return a + b; }

inline i128 i128_sub(i128 a, i128 b) { return a - b; }

inline i128 i128_mul(i128 a, i128 b) { return a * b; }

inline i128 i128_max(i128 a, i128 b) { return a > b ? a : b; }

inline i128 i128_min(i128 a, i128 b) { return a < b ? a : b; }

inline std::int64_t i128_to_i64(i128 v) {
    if ((v.bits.hi >> 63) == 0) return static_cast<std::int64_t>(v.bits.lo);
    u128 a = u128_abs(v);
    if (a.hi != 0 || a.lo > 0x8000000000000000ULL) return static_cast<std::int64_t>(v.bits.lo);
    return -static_cast<std::int64_t>(a.lo);
}

inline double i128_to_double(i128 v) { return static_cast<double>(v); }

inline i128 i128_from_i64(std::int64_t v) { return i128(v); }

inline i128 i128_from_u64(std::uint64_t v) { return i128(u128(v)); }

inline i128 i128_shl(i128 v, int n) {
    if (n <= 0) return v;
    if (n >= 128) return i128(0);
    u128 b = v.bits;
    if (n >= 64) return i128(u128(b.lo << (n - 64), 0));
    return i128(u128((b.hi << n) | (b.lo >> (64 - n)), b.lo << n));
}

inline i128 i128_shr(i128 v, int n) {
    if (n <= 0) return v;
    if (n >= 128) return v.bits.hi >> 63 ? i128(-1) : i128(0);
    u128 b = v.bits;
    if (n >= 64) {
        const std::uint64_t hi = b.hi >> (n - 64);
        const std::uint64_t lo = b.hi << (128 - n);
        return i128(u128(hi, lo));
    }
    return i128(u128((b.hi << (64 - n)) | (b.lo >> n), b.lo >> n));
}

inline i128 i128_and(i128 a, i128 b) { return i128(u128(a.bits.lo & b.bits.lo, a.bits.hi & b.bits.hi)); }

inline i128 i128_from_u128(u128 v) { return i128(v); }

inline u128 i128_to_u128(i128 v) { return v.bits; }

inline u128 u128_shl(u128 v, unsigned n) { return v << n; }

inline u128 u128_shr(u128 v, unsigned n) { return v >> n; }

inline u128 u128_mask(unsigned w) { return w >= 128 ? ~u128(0) : u128_shl(u128(1), w) - u128(1); }

inline bool u128_bit(u128 v, unsigned k) { return k < 128 && ((u128_shr(v, k).lo) & 1); }

inline std::string format_i128(i128 v) {
    if (v == i128(0)) return "0";
    bool neg = v.bits.hi >> 63;
    u128 u = u128_abs(v);
    std::string s;
    while (u != u128(0)) {
        auto qr = wide_int_detail::divmod_u128_u64(u, 10);
        u = qr.first;
        s += static_cast<char>('0' + static_cast<int>(qr.second));
    }
    if (neg) s += '-';
    return {s.rbegin(), s.rend()};
}

#endif  // PRISM_HAS_INT128

// Python int(text, 0): optional sign; 0x/0o/0b or decimal without bogus leading zeros.
inline std::optional<i128> parse_i128(std::string_view t) {
    std::size_t i = 0;
    bool neg = false;
    if (i < t.size() && (t[i] == '+' || t[i] == '-')) neg = t[i++] == '-';
    int base = 10;
    if (i + 1 < t.size() && t[i] == '0') {
        char p = static_cast<char>(std::tolower(static_cast<unsigned char>(t[i + 1])));
        if (p == 'x') base = 16;
        else if (p == 'o') base = 8;
        else if (p == 'b') base = 2;
        if (base != 10) {
            i += 2;
            if (i < t.size() && t[i] == '_') ++i;
        }
    }
    if (i >= t.size()) return std::nullopt;
    const std::size_t first = i;
#if PRISM_HAS_INT128
    u128 v = 0;
    const u128 limit = static_cast<u128>(1) << 127;
#else
    u128 v;
    const u128 limit{1ull << 63, 0};  // 2^127
#endif
    bool nonzero_lead = false;
    for (; i < t.size(); ++i) {
        char c = t[i];
        if (c == '_') {
            if (i == first || i + 1 >= t.size() || t[i + 1] == '_') return std::nullopt;
            continue;
        }
        int d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else return std::nullopt;
        if (d >= base) return std::nullopt;
        if (base == 10 && i == first && d != 0) nonzero_lead = true;
#if PRISM_HAS_INT128
        v = v * static_cast<unsigned>(base) + static_cast<unsigned>(d);
        if (v > limit) return std::nullopt;
#else
        u128 next = wide_int_detail::mul_u128_u64(v, static_cast<std::uint64_t>(base)) +
                    u128(static_cast<std::uint64_t>(d));
        if (next > limit) return std::nullopt;
        v = next;
#endif
    }
    if (base == 10 && !nonzero_lead) {
        for (std::size_t k = first; k < t.size(); ++k)
            if (t[k] != '0' && t[k] != '_') return std::nullopt;
    }
    if (!neg && v == limit) return std::nullopt;
#if PRISM_HAS_INT128
    i128 s = neg ? -static_cast<i128>(v - 1) - 1 : static_cast<i128>(v);
    if (neg && v == 0) s = 0;
#else
    i128 s;
    if (!neg) {
        s = i128(v);
    } else if (v == u128(0)) {
        s = i128(0);
    } else {
        s = -i128(v);
    }
#endif
    return s;
}

}  // namespace prism
