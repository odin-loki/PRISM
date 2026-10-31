// PRISM exact binary floating-point text (shared by the <format> model, the
// decimal shim's printf and the translator's printf-length planner).
//
// Every finite double is m * 2^e with integer m, so its decimal expansion is
// finite: N = m * 5^-e (e < 0) or m * 2^e (e >= 0), value = N * 10^min(e,0).
// The full expansion is computed with a small fixed-size bignum and then
// rounded half-even at the requested digit (the result glibc printf and
// std::to_chars give in the default rounding mode). Shortest round-trip
// (std::format "{}", to_chars without precision) picks the fewest
// significant digits whose value lies inside the rounding interval of the
// double, decided exactly against the two midpoints (2m-1)*2^(e-1) and
// (2m+1)*2^(e-1) (interval closed when m is even: ties-to-even reads back
// to the same double). Hex ('a') is exact from the bits. No libc, no heap:
// usable inside a pir model and in host code alike.
#ifndef PRISM_FPFMT_H
#define PRISM_FPFMT_H 1

namespace prism_fpfmt {

template <int N = 800>
struct DecT {          // value = digits * 10^x, digits[0] != '0' unless zero
    char d[N];
    int n = 0;
    int x = 0;
};

using Dec = DecT<800>;
// long double (x86 80-bit): up to ~11500 significant digits, 5^16445 * 2^64
using DecL = DecT<12000>;

template <int NW = 96>
struct BigT {          // little-endian base 2^32
    unsigned w[NW];
    int n = 1;
    BigT() { w[0] = w[1] = 0; }  // limbs at and above n are written before they are read
    void mul(unsigned k) {
        unsigned long long c = 0;
        for (int i = 0; i < n; ++i) { c += (unsigned long long)w[i] * k; w[i] = (unsigned)c; c >>= 32; }
        if (c && n < NW) w[n++] = (unsigned)c;
    }
    unsigned divmod(unsigned k) {   // this /= k, returns remainder
        unsigned long long r = 0;
        for (int i = n - 1; i >= 0; --i) { r = (r << 32) | w[i]; w[i] = (unsigned)(r / k); r %= k; }
        while (n > 1 && w[n - 1] == 0) --n;
        return (unsigned)r;
    }
    bool zero() const { return n == 1 && w[0] == 0; }
};

// Exact decimal of m * 2^e (m < 2^62).
template <int N>
inline void exact(unsigned long long m, int e, DecT<N>& out) {
    BigT<(N > 800 ? 1200 : 96)> b;
    b.w[0] = (unsigned)m; b.w[1] = (unsigned)(m >> 32); b.n = b.w[1] ? 2 : 1;
    out.x = 0;
    // Bounded trip counts for the translator's unwinder: multiply by 2^31
    // or 5^13 per step (both fit one 32-bit limb), so the tiniest
    // subnormal (e = -1074) takes 83 + 12 steps instead of 1074, and the
    // largest double (e = 971) 32 + 30 instead of 971.
    if (e >= 0) { int r = e; for (; r >= 31; r -= 31) b.mul(1u << 31); if (r) b.mul(1u << r); }
    else { int r = -e; for (; r >= 13; r -= 13) b.mul(1220703125u); unsigned p5 = 1; for (; r > 0; --r) p5 *= 5; if (p5 > 1) b.mul(p5); out.x = e; }
    char tmp[N];
    int k = 0;
    if (b.zero()) tmp[k++] = '0';
    while (!b.zero() && k < N - 10) {
        unsigned r = b.divmod(1000000000u);
        for (int j = 0; j < 9 && k < N - 1; ++j) { tmp[k++] = char('0' + r % 10); r /= 10; }
    }
    while (k > 1 && tmp[k - 1] == '0') --k;
    out.n = k;
    for (int i = 0; i < k; ++i) out.d[i] = tmp[k - 1 - i];
    // strip trailing zeros into the exponent
    while (out.n > 1 && out.d[out.n - 1] == '0') { --out.n; ++out.x; }
    if (out.n == 1 && out.d[0] == '0') out.x = 0;
}

// Keep `keep` leading significant digits, rounding half-even. keep <= 0
// rounds to zero or to one unit at that position. Returns false when the
// value rounds to zero (keep < 0 or a tie to even zero).
// mode: 0 nearest-even, 1 toward -inf, 2 toward +inf, 3 toward zero,
// 4 nearest-away (the C23 FE_DEC_* order); neg is the sign of the value.
template <int N>
inline void round_to(DecT<N>& v, int keep, int mode = 0, bool neg = false) {
    if (keep >= v.n) return;
    int sci = v.n - 1 + v.x;
    bool any = false;
    for (int i = keep < 0 ? 0 : keep; i < v.n; ++i) if (v.d[i] != '0') any = true;
    if (keep < 0) {
        // every digit is below the kept position: directed modes still step
        bool st = any && ((mode == 1 && neg) || (mode == 2 && !neg));
        if (st) { v.n = 1; v.d[0] = '1'; v.x = sci - keep + 1; return; }  // one unit of the kept place
        v.n = 1; v.d[0] = '0'; v.x = 0; return;
    }
    bool up;
    char f = v.d[keep];
    if (mode == 1) up = any && neg;
    else if (mode == 2) up = any && !neg;
    else if (mode == 3) up = false;
    else if (mode == 4) up = f >= '5';
    else if (f > '5') up = true;
    else if (f < '5') up = false;
    else {
        bool rest = false;
        for (int i = keep + 1; i < v.n; ++i) if (v.d[i] != '0') rest = true;
        up = rest || (keep > 0 && ((v.d[keep - 1] - '0') & 1));
    }
    v.x += v.n - keep;
    v.n = keep;
    if (keep == 0) {
        if (up) { v.d[0] = '1'; v.n = 1; v.x = sci + 1; }
        else { v.d[0] = '0'; v.n = 1; v.x = 0; }
        return;
    }
    if (up) {
        int i = keep - 1;
        while (i >= 0 && v.d[i] == '9') { v.d[i] = '0'; --i; }
        if (i >= 0) ++v.d[i];
        else { for (int j = v.n; j > 0; --j) v.d[j] = v.d[j - 1]; v.d[0] = '1'; ++v.n; }
    }
    while (v.n > 1 && v.d[v.n - 1] == '0') { --v.n; ++v.x; }
}

// -1/0/1 comparing two non-negative exact decimals.
template <int N>
inline int cmp(const DecT<N>& a, const DecT<N>& b) {
    bool az = a.n == 1 && a.d[0] == '0', bz = b.n == 1 && b.d[0] == '0';
    if (az || bz) return az && bz ? 0 : az ? -1 : 1;
    int ea = a.n - 1 + a.x, eb = b.n - 1 + b.x;
    if (ea != eb) return ea < eb ? -1 : 1;
    int l = a.n > b.n ? a.n : b.n;
    for (int i = 0; i < l; ++i) {
        char ca = i < a.n ? a.d[i] : '0', cb = i < b.n ? b.d[i] : '0';
        if (ca != cb) return ca < cb ? -1 : 1;
    }
    return 0;
}

struct Bits { bool neg; int kind; unsigned long long m; int e; int bits; };  // kind 0 finite, 1 inf, 2 nan

inline Bits decompose(double v) {
    unsigned long long u = __builtin_bit_cast(unsigned long long, v);
    Bits r{(u >> 63) != 0, 0, u & ((1ull << 52) - 1), 0, 53};
    int be = int((u >> 52) & 0x7ff);
    if (be == 0x7ff) { r.kind = r.m ? 2 : 1; return r; }
    if (be == 0) r.e = -1074;
    else { r.m |= 1ull << 52; r.e = be - 1075; }
    return r;
}
// x86 80-bit long double: explicit integer bit, 15-bit exponent
inline Bits decompose(long double v) {
    struct X { unsigned long long m; unsigned short se; unsigned short pad[3]; };
    static_assert(sizeof(long double) == sizeof(X), "x86 80-bit long double expected");
    X x = __builtin_bit_cast(X, v);
    Bits r{(x.se >> 15) != 0, 0, x.m, 0, 64};
    int be = x.se & 0x7fff;
    if (be == 0x7fff) { r.kind = (x.m << 1) ? 2 : 1; return r; }
    r.e = (be == 0 ? 1 : be) - 16383 - 63;
    return r;
}
inline Bits decompose(float v) {
    unsigned u = __builtin_bit_cast(unsigned, v);
    Bits r{(u >> 31) != 0, 0, u & ((1u << 23) - 1), 0, 24};
    int be = int((u >> 23) & 0xff);
    if (be == 0xff) { r.kind = r.m ? 2 : 1; return r; }
    if (be == 0) r.e = -149;
    else { r.m |= 1ull << 23; r.e = be - 150; }
    return r;
}

// Shortest digits that read back to the same binary value.
template <int N>
inline void shortest(const Bits& b, DecT<N>& out) {
    exact(b.m, b.e, out);
    if (b.m == 0) return;
    DecT<N> lo, hi;
    // lower neighbour gap halves at a power of two (not for the smallest normal's subnormal side)
    bool pow2 = b.m == (1ull << (b.bits - 1)) && b.e > (b.bits == 64 ? -16445 : b.bits == 53 ? -1074 : -149);
    if (pow2) exact(4 * b.m - 1, b.e - 2, lo); else exact(2 * b.m - 1, b.e - 1, lo);
    exact(2 * b.m + 1, b.e - 1, hi);
    const bool closed = (b.m & 1) == 0;
    DecT<N> full = out;
    for (int p = 1; p <= full.n; ++p) {
        DecT<N> c = full;
        round_to(c, p);
        int a = cmp(c, lo), z = cmp(c, hi);
        if ((a > 0 || (closed && a == 0)) && (z < 0 || (closed && z == 0))) { out = c; return; }
    }
}

struct Out {
    char* p; int cap; int len = 0;
    void put(char c) { if (len < cap) p[len] = c; ++len; }
    void puts(const char* s) { while (*s) put(*s++); }
};

enum : unsigned { kUpper = 1, kAlt = 2, kPlus = 4, kSpace = 8, kPrintfExp = 16 };

inline void put_exp(Out& o, int x, bool upper, char ch) {
    o.put(upper ? char(ch - 32) : ch);
    o.put(x < 0 ? '-' : '+');
    int a = x < 0 ? -x : x;
    char t[6]; int k = 0;
    do { t[k++] = char('0' + a % 10); a /= 10; } while (a);
    if (ch == 'e' && k < 2) t[k++] = '0';
    while (k) o.put(t[--k]);
}
// digits of v in fixed notation with exactly `frac` fraction digits (frac < 0: as many as v has)
template <int N>
inline void put_fixed(Out& o, const DecT<N>& v, int frac, bool alt) {
    int ip = v.n + v.x;  // number of integer digits (may be <= 0)
    if (ip <= 0) o.put('0');
    else for (int i = 0; i < ip; ++i) o.put(i < v.n ? v.d[i] : '0');
    int have = v.x < 0 ? -v.x : 0;
    int f = frac < 0 ? have : frac;
    if (f > 0 || alt) o.put('.');
    for (int i = 0; i < f; ++i) { int idx = ip + i; o.put(idx >= 0 && idx < v.n ? v.d[idx] : '0'); }
}
template <int N>
inline void put_sci(Out& o, const DecT<N>& v, int frac, bool alt, bool upper) {
    bool z = v.n == 1 && v.d[0] == '0';
    o.put(v.d[0]);
    int f = frac < 0 ? v.n - 1 : frac;
    if (f > 0 || alt) o.put('.');
    for (int i = 1; i <= f; ++i) o.put(i < v.n ? v.d[i] : '0');
    put_exp(o, z ? 0 : v.n - 1 + v.x, upper, 'e');
}

// type: 0 shortest (to_chars plain), 'e','f','g','a' (upper via flags or
// uppercase letter). prec < 0: shortest in that style ('e','f','a'), 6 for 'g'.
// Returns the full length; writes at most cap characters (no NUL).
template <typename F>
inline int to_chars(char* buf, int cap, F val, char type, int prec, unsigned flags) {
    Out o{buf, cap};
    if (type >= 'A' && type <= 'Z') { flags |= kUpper; type = char(type + 32); }
    const bool up = flags & kUpper, alt = flags & kAlt;
    Bits b = decompose(val);
    if (b.neg) o.put('-'); else if (flags & kPlus) o.put('+'); else if (flags & kSpace) o.put(' ');
    if (b.kind) { o.puts(b.kind == 1 ? (up ? "INF" : "inf") : (up ? "NAN" : "nan")); return o.len; }
    if (type == 'a') {
        // to_chars (std::format 'a') normalises subnormals; printf %a keeps 0x0.
        if (!(flags & kPrintfExp) && b.m && !(b.m >> (b.bits - 1)))
            while (!(b.m >> (b.bits - 1))) { b.m <<= 1; --b.e; }
        const int hexd = b.bits == 53 ? 13 : 6;  // fraction hex digits (float: 23 bits -> 6 digits, shifted)
        unsigned long long frac = b.bits == 53 ? (b.m & ((1ull << 52) - 1)) : ((b.m & ((1ull << 23) - 1)) << 1);
        int lead = b.m >> (b.bits - 1) ? 1 : 0;
        int ex = b.m == 0 ? 0 : (lead ? b.e + b.bits - 1 : (b.bits == 53 ? -1022 : -126));
        int nd = hexd;
        if (prec < 0) { while (nd > 0 && ((frac >> (4 * (hexd - nd))) & 0xf) == 0) --nd; }
        else if (prec < hexd) {
            int drop = 4 * (hexd - prec);
            unsigned long long rem = frac & ((1ull << drop) - 1), half = 1ull << (drop - 1);
            frac >>= drop;
            if (rem > half || (rem == half && (frac & 1))) ++frac;
            if (frac >> (4 * prec)) { frac = 0; ++lead; }
            frac <<= drop;
            nd = prec;
        } else nd = prec;
        if (flags & kPrintfExp) { o.put('0'); o.put(up ? 'X' : 'x'); }
        o.put(char('0' + lead));
        if (nd > 0 || alt) o.put('.');
        const char* hx = up ? "0123456789ABCDEF" : "0123456789abcdef";
        for (int i = 0; i < nd; ++i) o.put(i < hexd ? hx[(frac >> (4 * (hexd - 1 - i))) & 0xf] : '0');
        put_exp(o, ex, up, 'p');
        return o.len;
    }
    DecT<(sizeof(F) > 8 ? 12000 : 800)> v;
    if (type == 0 && prec < 0) {
        shortest(b, v);
        bool z = v.n == 1 && v.d[0] == '0';
        int sx = z ? 0 : v.n - 1 + v.x;
        // to_chars plain: scientific only when strictly shorter
        int fl = (v.n + v.x > 0 ? (v.n + v.x > v.n ? v.n + v.x : v.n) : 1) + (v.x < 0 ? 1 : 0) + (v.x < 0 && v.n + v.x <= 0 ? 0 : 0);
        if (v.x < 0) fl = (v.n + v.x > 0 ? v.n + v.x : 1) + 1 + (-v.x);
        int ax = sx < 0 ? -sx : sx;
        int sl = v.n + (v.n > 1 ? 1 : 0) + 2 + (ax >= 100 ? 3 : 2);
        if (sl < fl) put_sci(o, v, -1, alt, up);
        else { if (v.x > 0) exact(b.m, b.e, v); put_fixed(o, v, -1, alt); }  // integers print exactly
        return o.len;
    }
    if (type == 0) type = 'g';
    if ((type == 'e' || type == 'f') && prec < 0) {
        shortest(b, v);
        if (type == 'e') put_sci(o, v, -1, alt, up);
        else { if (v.x > 0) exact(b.m, b.e, v); put_fixed(o, v, -1, alt); }
        return o.len;
    }
    if (prec < 0) prec = 6;
    exact(b.m, b.e, v);
    bool z = v.n == 1 && v.d[0] == '0';
    if (type == 'e') { if (!z) round_to(v, prec + 1); put_sci(o, v, prec, alt, up); return o.len; }
    if (type == 'f') {
        if (!z) round_to(v, v.n + v.x + prec);  // keep digits down to 10^-prec
        put_fixed(o, v, prec, alt);
        return o.len;
    }
    // 'g' (C17 7.21.6.1p8)
    int P = prec == 0 ? 1 : prec;
    auto r = v;
    if (!z) round_to(r, P);
    int X = z ? 0 : r.n - 1 + r.x;
    char t[1100];
    Out q{t, 1100};
    if (P > X && X >= -4) {
        auto f = v;
        if (!z) round_to(f, f.n + f.x + (P - 1 - X));
        put_fixed(q, f, P - 1 - X, alt);
    } else put_sci(q, r, P - 1, alt, up);
    int n = q.len < 1100 ? q.len : 1100;
    if (!alt) {  // strip trailing zeros of the fraction (and a bare '.')
        int epos = n;
        for (int i = 0; i < n; ++i) if (t[i] == 'e' || t[i] == 'E') { epos = i; break; }
        int dot = -1;
        for (int i = 0; i < epos; ++i) if (t[i] == '.') dot = i;
        if (dot >= 0) {
            int k = epos;
            while (k > dot + 1 && t[k - 1] == '0') --k;
            if (k == dot + 1) k = dot;
            for (int i = 0; i < k; ++i) o.put(t[i]);
            for (int i = epos; i < n; ++i) o.put(t[i]);
            return o.len;
        }
    }
    for (int i = 0; i < n; ++i) o.put(t[i]);
    return o.len;
}

}  // namespace prism_fpfmt
#endif
