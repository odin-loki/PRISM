// prism_dfp_mp.hpp: correctly rounded decimal transcendental functions
// (coverage item 4). A multi-limb decimal float (base 1e9 limbs, value =
// sum d[i] * 1e9^(i+q)) evaluates each function at W digits; Ziv's loop
// raises W until rounding the result to the format's P digits cannot be
// changed by the evaluation error (the digits after P+1, minus a 12-digit
// error margin and any digits lost to cancellation, are neither all 0 nor
// all 9). At the precision cap the result is taken as exact (exp(0),
// log10(1000), pow(2,10), cbrt(27), ...). Tiny arguments use the sign of
// the first neglected Taylor term. erf/erfc/tgamma/lgamma still use the
// long double path. Included by prism_dfp.hpp inside namespace __prism_dfpm.
#pragma once
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// a minimal growable array (the shim includes no C++ library headers)
template <typename T> struct _Vec {
    T* p = nullptr; size_t n = 0, cap = 0;
    _Vec() = default;
    _Vec(size_t k, T v) { assign(k, v); }
    _Vec(const _Vec& o) { *this = o; }
    _Vec& operator=(const _Vec& o) { if (this != &o) { reserve(o.n); if (o.n) memcpy(p, o.p, o.n * sizeof(T)); n = o.n; } return *this; }
    ~_Vec() { free(p); }
    void reserve(size_t k) { if (k > cap) { size_t c = cap ? cap : 8; while (c < k) c *= 2; p = (T*)realloc(p, c * sizeof(T)); cap = c; } }
    size_t size() const { return n; }
    bool empty() const { return n == 0; }
    T& operator[](size_t i) { return p[i]; }
    const T& operator[](size_t i) const { return p[i]; }
    T& back() { return p[n - 1]; }
    const T& back() const { return p[n - 1]; }
    void push_back(T v) { reserve(n + 1); p[n++] = v; }
    void pop_back() { --n; }
    void clear() { n = 0; }
    void assign(size_t k, T v) { reserve(k); for (size_t i = 0; i < k; ++i) p[i] = v; n = k; }
    void erase_front(size_t k) { if (k >= n) { n = 0; return; } memmove(p, p + k, (n - k) * sizeof(T)); n -= k; }
    void insert_front(size_t k, T v) { reserve(n + k); memmove(p + k, p, n * sizeof(T)); for (size_t i = 0; i < k; ++i) p[i] = v; n += k; }
};

struct _Mp {
    bool neg = false;
    long q = 0;
    _Vec<uint32_t> d;  // little endian, base 1e9; empty = 0
    bool zero() const { return d.empty(); }
};
static constexpr uint32_t _Mb = 1000000000u;
inline void _mp_norm(_Mp& a) {
    while (!a.d.empty() && a.d.back() == 0) a.d.pop_back();
    size_t z = 0; while (z < a.d.size() && a.d[z] == 0) ++z;
    if (z) { a.d.erase_front(z); a.q += (long)z; }
    if (a.d.empty()) { a.q = 0; a.neg = false; }
}
inline void _mp_trunc(_Mp& a, int L) {
    _mp_norm(a);
    if ((int)a.d.size() > L) { size_t k = a.d.size() - (size_t)L; a.d.erase_front(k); a.q += (long)k; }
}
inline _Mp _mp_u(unsigned long long v) { _Mp r; while (v) { r.d.push_back((uint32_t)(v % _Mb)); v /= _Mb; } return r; }
inline long _mp_top(const _Mp& a) { return a.q + (long)a.d.size() - 1; }  // limb exponent of the top limb
inline int _mp_cmpabs(const _Mp& a, const _Mp& b) {
    if (a.zero() || b.zero()) return a.zero() ? (b.zero() ? 0 : -1) : 1;
    if (_mp_top(a) != _mp_top(b)) return _mp_top(a) < _mp_top(b) ? -1 : 1;
    long lo = a.q < b.q ? a.q : b.q;
    for (long e = _mp_top(a); e >= lo; --e) {
        uint32_t x = e >= a.q ? a.d[(size_t)(e - a.q)] : 0, y = e >= b.q ? b.d[(size_t)(e - b.q)] : 0;
        if (x != y) return x < y ? -1 : 1;
    }
    return 0;
}
inline _Mp _mp_addabs(const _Mp& a, const _Mp& b, bool sub) {  // |a| + |b| or |a| - |b| (|a| >= |b|)
    _Mp r; r.q = a.q < b.q ? a.q : b.q;
    long hi = (a.zero() ? b.q : _mp_top(a)) > (b.zero() ? a.q : _mp_top(b)) ? (a.zero() ? b.q : _mp_top(a)) : (b.zero() ? a.q : _mp_top(b));
    r.d.assign((size_t)(hi - r.q + 2), 0);
    long long c = 0;
    for (long e = r.q; e <= hi + 1; ++e) {
        long long x = (e >= a.q && e - a.q < (long)a.d.size()) ? a.d[(size_t)(e - a.q)] : 0;
        long long y = (e >= b.q && e - b.q < (long)b.d.size()) ? b.d[(size_t)(e - b.q)] : 0;
        long long s = sub ? x - y + c : x + y + c;
        c = 0;
        if (s < 0) { s += _Mb; c = -1; } else if (s >= _Mb) { s -= _Mb; c = 1; }
        r.d[(size_t)(e - r.q)] = (uint32_t)s;
    }
    _mp_norm(r);
    return r;
}
inline _Mp _mp_add(const _Mp& a, const _Mp& b, int L) {
    _Mp r;
    if (a.neg == b.neg) { r = _mp_addabs(a, b, false); r.neg = a.neg; }
    else if (_mp_cmpabs(a, b) >= 0) { r = _mp_addabs(a, b, true); r.neg = a.neg; }
    else { r = _mp_addabs(b, a, true); r.neg = b.neg; }
    _mp_trunc(r, L); if (r.zero()) r.neg = false;
    return r;
}
inline _Mp _mp_neg(_Mp a) { if (!a.zero()) a.neg = !a.neg; return a; }
inline _Mp _mp_sub(const _Mp& a, const _Mp& b, int L) { return _mp_add(a, _mp_neg(b), L); }
inline _Mp _mp_mul(const _Mp& a, const _Mp& b, int L) {
    if (a.zero() || b.zero()) return _Mp();
    // only the top L+2 limbs of each operand matter at precision L
    size_t sa = a.d.size() > (size_t)L + 2 ? a.d.size() - (size_t)L - 2 : 0, sb = b.d.size() > (size_t)L + 2 ? b.d.size() - (size_t)L - 2 : 0;
    _Vec<unsigned long long> t(a.d.size() - sa + b.d.size() - sb + 1, 0ull);
    for (size_t i = sa; i < a.d.size(); ++i) {
        unsigned long long c = 0;
        for (size_t j = sb; j < b.d.size(); ++j) {
            unsigned long long v = t[i - sa + j - sb] + (unsigned long long)a.d[i] * b.d[j] + c;
            t[i - sa + j - sb] = v % _Mb; c = v / _Mb;
        }
        for (size_t k = i - sa + b.d.size() - sb; c; ++k) { unsigned long long v = t[k] + c; t[k] = v % _Mb; c = v / _Mb; }
    }
    _Mp r; r.neg = a.neg != b.neg; r.q = a.q + (long)sa + b.q + (long)sb;
    r.d.assign(t.size(), 0u); for (size_t i = 0; i < t.size(); ++i) r.d[i] = (uint32_t)t[i];
    _mp_trunc(r, L);
    return r;
}
inline _Mp _mp_muls(const _Mp& a, uint32_t k, int L) { _Mp b = _mp_u(k); return _mp_mul(a, b, L); }
inline _Mp _mp_divs(_Mp a, uint32_t k, int L) {  // a / k, k < 2^32
    if (a.zero()) return a;
    long pad = (long)L + 2 - (long)a.d.size();
    if (pad > 0) { a.d.insert_front((size_t)pad, 0u); a.q -= pad; }
    unsigned long long r = 0;
    for (size_t i = a.d.size(); i-- > 0;) { unsigned long long v = r * _Mb + a.d[i]; a.d[i] = (uint32_t)(v / k); r = v % k; }
    _mp_trunc(a, L);
    return a;
}
inline double _mp_dbl(const _Mp& a, long* ex) {  // a = ret * 1e9^ex, ret in [1, 1e9)
    if (a.zero()) { *ex = 0; return 0; }
    double v = a.d.back();
    if (a.d.size() > 1) v += a.d[a.d.size() - 2] / 1e9;
    if (a.d.size() > 2) v += a.d[a.d.size() - 3] / 1e18;
    *ex = _mp_top(a);
    return a.neg ? -v : v;
}
inline _Mp _mp_fromd(double v, long ex) {  // v * 1e9^ex, 18 significant digits
    _Mp r; r.neg = v < 0; if (v < 0) v = -v;
    unsigned long long m = (unsigned long long)(v * 1e9);  // v < 1e9 -> < 1e18
    r = _mp_u(m); r.neg = v != 0 && r.neg; r.q = ex - 1;
    _mp_norm(r);
    return r;
}
inline _Mp _mp_recip(const _Mp& b, int L) {
    long ex; double t = _mp_dbl(b, &ex);
    _Mp r = _mp_fromd(1e9 / t, -ex - 1);  // 1/t in (1e-9, 1]: scaled by 1e9
    _Mp one = _mp_u(1);
    for (int p = 2; ; p = p * 2 < L + 2 ? p * 2 : L + 2) {
        _Mp e = _mp_sub(one, _mp_mul(b, r, p + 2), p + 2);
        r = _mp_add(r, _mp_mul(r, e, p + 2), p + 2);
        if (p == L + 2) break;
    }
    _mp_trunc(r, L);
    return r;
}
inline _Mp _mp_div(const _Mp& a, const _Mp& b, int L) { return _mp_mul(a, _mp_recip(b, L + 1), L); }
inline _Mp _mp_sqrt(const _Mp& a, int L) {  // a > 0
    long ex; double t = _mp_dbl(a, &ex);
    if (ex & 1) { t *= 1e9; --ex; }
    _Mp y = _mp_fromd(1e9 / __builtin_sqrt(t) / (t >= 1e9 ? 1 : 1), -ex / 2 - 1);  // ~ 1/sqrt(a)
    _Mp one = _mp_u(1);
    for (int p = 2; ; p = p * 2 < L + 2 ? p * 2 : L + 2) {
        _Mp e = _mp_sub(one, _mp_mul(a, _mp_mul(y, y, p + 2), p + 2), p + 2);
        y = _mp_add(y, _mp_divs(_mp_mul(y, e, p + 2), 2, p + 2), p + 2);
        if (p == L + 2) break;
    }
    return _mp_mul(a, y, L);
}
inline _Mp _mp_shift10(_Mp a, long n, int L) {  // a * 10^n
    long r = ((n % 9) + 9) % 9, qd = (n - r) / 9;
    static const uint32_t p10[9] = {1, 10, 100, 1000, 10000, 100000, 1000000, 10000000, 100000000};
    if (r) a = _mp_muls(a, p10[r], L + 1);
    a.q += qd;
    _mp_trunc(a, L);
    return a;
}
template <typename _W> inline _Mp _mp_from(const _W& w) {
    unsigned __int128 c = w._M_c < 0 ? (unsigned __int128)(-w._M_c) : (unsigned __int128)w._M_c;
    _Mp r; while (c) { r.d.push_back((uint32_t)(c % _Mb)); c /= _Mb; }
    r.neg = w._M_c < 0;
    _mp_norm(r);
    return _mp_shift10(r, w._M_e, 1 << 20);
}
inline long _mp_dexp(const _Mp& a) {  // decimal exponent of the leading digit
    long e = 9 * _mp_top(a); for (uint32_t v = a.d.back(); v >= 10; v /= 10) ++e; return e;
}
// constants: sum of 1/((2k+1) m^(2k+1)), alternating for atan
inline _Mp _mp_series_inv(uint32_t m, bool alt, int L) {
    _Mp term = _mp_divs(_mp_u(1), m, L + 1), s = term;
    const uint32_t m2 = m * m;
    for (uint32_t k = 1; ; ++k) {
        term = _mp_divs(term, m2, L + 1);
        if (term.zero() || _mp_top(term) < _mp_top(s) - L - 1) break;
        _Mp t = _mp_divs(term, 2 * k + 1, L + 1);
        s = (alt && (k & 1)) ? _mp_sub(s, t, L + 1) : _mp_add(s, t, L + 1);
    }
    return s;
}
inline const _Mp& _mp_ln2(int L) { static _Mp c; static int cl = 0; if (cl < L) { c = _mp_muls(_mp_series_inv(3, false, L + 1), 2, L + 1); cl = L; } return c; }
inline const _Mp& _mp_ln10(int L) { static _Mp c; static int cl = 0; if (cl < L) { c = _mp_add(_mp_muls(_mp_ln2(L + 1), 3, L + 1), _mp_muls(_mp_series_inv(9, false, L + 1), 2, L + 1), L + 1); cl = L; } return c; }
inline const _Mp& _mp_pi(int L) { static _Mp c; static int cl = 0; if (cl < L) { c = _mp_sub(_mp_muls(_mp_series_inv(5, true, L + 1), 16, L + 1), _mp_muls(_mp_series_inv(239, true, L + 1), 4, L + 1), L + 1); cl = L; } return c; }

inline _Mp _mp_exp_small(const _Mp& r, int L) {  // |r| <~ 1.2
    const int s = 20;
    _Mp x = r; for (int i = 0; i < s; ++i) x = _mp_divs(x, 2, L + 2);
    _Mp sum = _mp_u(1), term = _mp_u(1);
    for (uint32_t k = 1; ; ++k) {
        term = _mp_divs(_mp_mul(term, x, L + 2), k, L + 2);
        if (term.zero() || _mp_top(term) < -(long)L - 3) break;
        sum = _mp_add(sum, term, L + 2);
    }
    for (int i = 0; i < s; ++i) sum = _mp_mul(sum, sum, L + 2);
    return sum;
}
// exp(x); *ovf = +1 / -1 when out of every decimal range
inline _Mp _mp_exp(const _Mp& x, int L, int* ovf) {
    long ex; double xd = _mp_dbl(x, &ex);
    *ovf = 0;
    if (ex >= 1) { *ovf = x.neg ? -1 : 1; return _Mp(); }
    const double xv = ex == 0 ? xd : xd / 1e9;
    if (xv > 14300) { *ovf = 1; return _Mp(); }
    if (xv < -14400) { *ovf = -1; return _Mp(); }
    const long k = (long)__builtin_llround(xv / 2.302585092994046);
    _Mp r = _mp_sub(x, _mp_mul(_mp_u((unsigned long long)(k < 0 ? -k : k)), _mp_ln10(L + 2), L + 2), L + 2);
    if (k < 0) r = _mp_add(x, _mp_mul(_mp_u((unsigned long long)-k), _mp_ln10(L + 2), L + 2), L + 2);
    return _mp_shift10(_mp_exp_small(r, L), k, L);
}
inline _Mp _mp_atanh_series(const _Mp& z, int L) {  // atanh z, |z| < 1/2
    _Mp z2 = _mp_mul(z, z, L + 1), p = z, s = z;
    for (uint32_t k = 1; ; ++k) {
        p = _mp_mul(p, z2, L + 1);
        if (p.zero() || _mp_top(p) < _mp_top(s) - L - 2) break;
        s = _mp_add(s, _mp_divs(p, 2 * k + 1, L + 1), L + 1);
    }
    return s;
}
inline _Mp _mp_log(const _Mp& x, int L) {  // x > 0
    _Mp one = _mp_u(1);
    long E = _mp_dexp(x);
    _Mp f = _mp_shift10(x, -E, L + 2);  // [1, 10)
    long ex; double fd = _mp_dbl(f, &ex);
    if (E == -1 && fd * 1e-9 >= 0.5) { f = x; E = 0; }  // x in [0.5, 1): no reduction
    int j = 0;
    if (E != 0 || fd > 2) { while (_mp_dbl(f, &ex) > 1.42 && ex == 0) { f = _mp_divs(f, 2, L + 2); ++j; } }
    _Mp z = _mp_div(_mp_sub(f, one, L + 2), _mp_add(f, one, L + 2), L + 2);
    _Mp r = _mp_muls(_mp_atanh_series(z, L + 2), 2, L + 2);
    if (j) r = _mp_add(r, _mp_muls(_mp_ln2(L + 2), (uint32_t)j, L + 2), L + 2);
    if (E) { _Mp t = _mp_muls(_mp_ln10(L + 2), (uint32_t)(E < 0 ? -E : E), L + 2); r = E < 0 ? _mp_sub(r, t, L + 2) : _mp_add(r, t, L + 2); }
    _mp_trunc(r, L);
    return r;
}
inline void _mp_sincos_small(const _Mp& r, int L, _Mp* s, _Mp* c) {
    _Mp r2 = _mp_mul(r, r, L + 1);
    if (s) { _Mp t = r, a = r; for (uint32_t k = 1; ; ++k) { t = _mp_neg(_mp_divs(_mp_mul(t, r2, L + 1), (2 * k) * (2 * k + 1), L + 1)); if (t.zero() || _mp_top(t) < _mp_top(a) - L - 2) break; a = _mp_add(a, t, L + 1); } *s = a; }
    if (c) { _Mp t = _mp_u(1), a = t; for (uint32_t k = 1; ; ++k) { t = _mp_neg(_mp_divs(_mp_mul(t, r2, L + 1), (2 * k - 1) * (2 * k), L + 1)); if (t.zero() || _mp_top(t) < -(long)L - 2) break; a = _mp_add(a, t, L + 1); } *c = a; }
}
// x = n*pi/2 + r, |r| <= pi/4; returns n mod 4; *lost = digits lost to cancellation
inline int _mp_reduce(const _Mp& x, int L, _Mp* r, long* lost) {
    const long mag = _mp_dexp(x) > 0 ? _mp_dexp(x) / 9 + 1 : 0;
    const int LL = L + (int)mag + 2;
    _Mp t = _mp_mul(x, _mp_div(_mp_u(2), _mp_pi(LL), LL), LL);
    bool neg = t.neg; t.neg = false;
    _Mp ip, fp = t;  // integer / fraction limbs
    fp.d.clear(); ip.d.clear(); ip.q = 0; fp.q = t.q;
    for (size_t i = 0; i < t.d.size(); ++i) { long e = t.q + (long)i; if (e >= 0) { if (ip.d.empty()) ip.q = e; while (ip.q + (long)ip.d.size() < e) ip.d.push_back(0); ip.d.push_back(t.d[i]); } else fp.d.push_back(t.d[i]); }
    _mp_norm(fp);
    uint32_t n4 = 0;
    if (!ip.d.empty() && ip.q == 0) n4 = ip.d[0] % 4;
    long ex; double fd = _mp_dbl(fp, &ex);
    if (!fp.zero() && ex == -1 && fd >= 5e8) { fp = _mp_sub(fp, _mp_u(1), LL); n4 = (n4 + 1) % 4; }
    *lost = fp.zero() ? 1000000 : -_mp_dexp(fp);
    if (neg) { fp = _mp_neg(fp); n4 = (4 - n4) % 4; }
    *r = _mp_mul(fp, _mp_divs(_mp_pi(LL), 2, LL), L);
    return (int)n4;
}
inline _Mp _mp_atan(_Mp x, int L) {
    bool inv = false, neg = x.neg; x.neg = false;
    _Mp one = _mp_u(1);
    if (_mp_cmpabs(x, one) > 0) { x = _mp_div(one, x, L + 2); inv = true; }
    int h = 0;
    for (; h < 4; ++h) x = _mp_div(x, _mp_add(one, _mp_sqrt(_mp_add(one, _mp_mul(x, x, L + 2), L + 2), L + 2), L + 2), L + 2);
    _Mp x2 = _mp_mul(x, x, L + 2), p = x, s = x;
    for (uint32_t k = 1; ; ++k) { p = _mp_neg(_mp_mul(p, x2, L + 2)); if (p.zero() || _mp_top(p) < _mp_top(s) - L - 2) break; s = _mp_add(s, _mp_divs(p, 2 * k + 1, L + 2), L + 2); }
    s = _mp_muls(s, 1u << h, L + 2);
    if (inv) s = _mp_sub(_mp_divs(_mp_pi(L + 2), 2, L + 2), s, L + 2);
    if (neg) s = _mp_neg(s);
    return s;
}

// ---- Ziv driver ----
template <typename _D> inline int _mp_P() { int p = 0; for (__int128 l = _Wt<_D>::_S_lim; l > 1; l /= 10) ++p; return p; }
// round an MP value with `good` trusted digits to _D; *ok = false if ambiguous
template <typename _D> inline _D _mp_round(const _Mp& v, long good, bool force, bool* ok) {
    const int P = _mp_P<_D>();
    char s[16]; _Vec<char> dg;
    for (size_t i = v.d.size(); i-- > 0;) { snprintf(s, sizeof s, i + 1 == v.d.size() ? "%u" : "%09u", v.d[i]); for (char* p = s; *p; ++p) dg.push_back(*p); }
    const long n0 = (long)dg.size();
    while ((long)dg.size() < P + 3) dg.push_back('0');
    const long len = (long)dg.size();
    const long el = 9 * v.q - (len - n0);  // exponent of the last digit
    good -= 9 - (long)snprintf(s, sizeof s, "%u", v.d.back());  // the top limb's missing digits
    long end = good < len ? good : len;
    bool all0 = true, all9 = true;
    for (long i = P + 1; i < end; ++i) { if (dg[(size_t)i] != '0') all0 = false; if (dg[(size_t)i] != '9') all9 = false; }
    *ok = force || (end > P + 1 && !all0 && !all9);
    if (!*ok) return _D();
    __int128 c = 0;
    for (int i = 0; i <= P; ++i) c = c * 10 + (dg[(size_t)i] - '0');
    int st = 1;
    if (all0) st = 0;
    if (all9) { c += 1; st = 0; }
    c = c * 10 + st;
    const long e = el + (len - 1 - (P + 1));
    return _D(_Wt<_D>::_S_make(v.neg ? -c : c, (int)e));
}
template <typename _D, typename _F> inline _D _mp_ziv(_F f, long extra_cap) {
    const int P = _mp_P<_D>();
    const long cap = 4 * (P + 40) + extra_cap;
    for (long W = P + 30; ; W *= 2) {
        const bool last = W >= cap;
        const int L = (int)(W / 9) + 2;
        long lost = 0; int special = 0;
        _Mp v = f(L, &lost, &special);
        if (special) return _D(special == 1 ? _Wt<_D>::_S_make(1, 20000) : special == 2 ? _Wt<_D>::_S_make(-1, 20000) : _Wt<_D>::_S_make(1, -20000));
        if (v.zero()) return _D(0);
        bool ok;
        _D r = _mp_round<_D>(v, 9 * L - 12 - lost - 9, last, &ok);
        if (ok) return r;
    }
}
// tiny argument: f(x) = x (1 + O(x^2)); dir = -1 smaller magnitude, +1 larger
template <typename _D> inline _D _mp_nudge(_D x, int dir) {
    auto w = x._M_w(); const int P = _mp_P<_D>();
    __int128 c = w._M_c; int dc = 0; for (__int128 t = c < 0 ? -c : c; t; t /= 10) ++dc;
    int sh = P + 2 - dc; for (int i = 0; i < sh; ++i) c *= 10;
    c += (c < 0 ? -1 : 1) * dir;
    return _D(_Wt<_D>::_S_make(c, w._M_e - sh));
}
template <typename _D> inline bool _mp_tiny(_D x, int k) {  // |x| < 10^-(P+3)/k
    auto w = x._M_w(); int dc = 0; for (__int128 t = w._M_c < 0 ? -w._M_c : w._M_c; t; t /= 10) ++dc;
    return w._M_e + dc - 1 < -(_mp_P<_D>() + 3) / k;
}
template <typename _D> inline bool _mp_fin(_D x) { auto w = x._M_w(); return w._M_k == 0; }
template <typename _D> inline bool _mp_z(_D x) { auto w = x._M_w(); return w._M_k == 0 && w._M_c == 0; }

template <typename _D> inline _D _cr_exp(_D x, int base /*0 e, 2, 10*/, bool m1) {
    _Mp X = _mp_from(x._M_w());
    if (base == 10) {  // exact for integers in range
        auto w = x._M_w();
        if (w._M_e >= 0 && w._M_c > -20000 && w._M_c < 20000) { long k = (long)w._M_c; for (int i = 0; i < w._M_e && k; ++i) k *= 10; if (k > -20000 && k < 20000 && !m1) return _D(_Wt<_D>::_S_make(1, (int)k)); }
    }
    return _mp_ziv<_D>([&](int L, long* lost, int* sp) {
        _Mp a = X;
        if (base == 2) a = _mp_mul(a, _mp_ln2(L + 2), L + 2);
        if (base == 10) a = _mp_mul(a, _mp_ln10(L + 2), L + 2);
        if (m1) {
            long ex; double ad = _mp_dbl(a, &ex);
            if (ex < 0 || (ex == 0 && ad < 0.5 && ad > -0.5)) {  // Taylor without cancellation
                _Mp s = a, t = a;
                for (uint32_t k = 2; ; ++k) { t = _mp_divs(_mp_mul(t, a, L + 1), k, L + 1); if (t.zero() || _mp_top(t) < _mp_top(s) - L - 2) break; s = _mp_add(s, t, L + 1); }
                return s;
            }
        }
        int o; _Mp e = _mp_exp(a, L, &o);
        if (o) { if (m1 && o < 0) { *sp = 0; return _mp_neg(_mp_u(1)); } *sp = o; return e; }
        if (m1) { e = _mp_sub(e, _mp_u(1), L); *lost = e.zero() ? 0 : (_mp_dexp(e) < 0 ? -_mp_dexp(e) : 0); }
        return e;
    }, 0);
}
template <typename _D> inline _D _cr_log(_D x, int base, bool p1) {
    _Mp X = _mp_from(x._M_w());
    if (p1) X = _mp_add(X, _mp_u(1), 1 << 20);
    if (X.zero()) return _D(_Wt<_D>::_S_inf(true));
    if (X.neg) return _D(_Wt<_D>::_S_nan());
    if (base == 10) {  // exact powers of ten
        _Mp t = X; _mp_norm(t);
        bool pw = true; uint32_t top = t.d.back();
        for (size_t i = 0; i + 1 < t.d.size(); ++i) if (t.d[i]) pw = false;
        while (pw && top % 10 == 0) top /= 10;
        if (pw && top == 1) return _D((long long)_mp_dexp(t));
    }
    return _mp_ziv<_D>([&](int L, long* lost, int*) {
        _Mp r;
        if (p1) {
            long ex; double xd = _mp_dbl(_mp_from(x._M_w()), &ex);
            if (ex < 0 || (ex == 0 && xd < 0.5 && xd > -0.5)) {
                _Mp a = _mp_from(x._M_w());
                r = _mp_muls(_mp_atanh_series(_mp_div(a, _mp_add(_mp_u(2), a, L + 2), L + 2), L + 2), 2, L + 2);
            } else r = _mp_log(X, L + 2);
        } else r = _mp_log(X, L + 2);
        if (base == 2) r = _mp_div(r, _mp_ln2(L + 2), L + 1);
        if (base == 10) r = _mp_div(r, _mp_ln10(L + 2), L + 1);
        *lost = r.zero() ? 0 : (_mp_dexp(r) < 0 ? -_mp_dexp(r) : 0);
        if (*lost > 4 * L) *lost = 0;
        return r;
    }, 0);
}
template <typename _D> inline _D _cr_trig(_D x, int which /*0 sin 1 cos 2 tan*/) {
    _Mp X = _mp_from(x._M_w());
    long xe = _mp_dexp(X); if (xe < 0) xe = 0;
    return _mp_ziv<_D>([&](int L, long* lost, int*) {
        _Mp r; int n = _mp_reduce(X, L + 1, &r, lost);
        if (*lost > 100000) *lost = 0;
        _Mp s, c; _mp_sincos_small(r, L + 1, &s, &c);
        if (which == 0) { _Mp v = (n & 1) ? c : s; return (n & 2) ? _mp_neg(v) : v; }
        if (which == 1) { _Mp v = (n & 1) ? s : c; return (n == 1 || n == 2) ? _mp_neg(v) : v; }
        return (n & 1) ? _mp_neg(_mp_div(c, s, L)) : _mp_div(s, c, L);
    }, 2 * xe);
}
template <typename _D> inline _D _cr_atan(_D x) { _Mp X = _mp_from(x._M_w()); return _mp_ziv<_D>([&](int L, long*, int*) { return _mp_atan(X, L); }, 0); }
template <typename _D> inline _D _cr_asin(_D x, bool ac) {
    _Mp X = _mp_from(x._M_w()), one = _mp_u(1);
    int c = _mp_cmpabs(X, one);
    if (c > 0) return _D(_Wt<_D>::_S_nan());
    return _mp_ziv<_D>([&](int L, long* lost, int*) {
        _Mp a;
        if (c == 0) { a = _mp_divs(_mp_pi(L + 2), 2, L + 2); if (X.neg) a = _mp_neg(a); }
        else a = _mp_atan(_mp_div(X, _mp_sqrt(_mp_sub(one, _mp_mul(X, X, L + 3), L + 3), L + 3), L + 3), L + 2);
        if (ac) { a = _mp_sub(_mp_divs(_mp_pi(L + 2), 2, L + 2), a, L + 1); *lost = a.zero() ? 0 : (_mp_dexp(a) < 0 ? -_mp_dexp(a) : 0); }
        return a;
    }, 0);
}
template <typename _D> inline _D _cr_atan2(_D y, _D x) {
    _Mp Y = _mp_from(y._M_w()), X = _mp_from(x._M_w());
    return _mp_ziv<_D>([&](int L, long*, int*) {
        _Mp a = _mp_atan(_mp_div(Y, X, L + 3), L + 2);
        if (X.neg) a = Y.neg ? _mp_sub(a, _mp_pi(L + 2), L + 1) : _mp_add(a, _mp_pi(L + 2), L + 1);
        return a;
    }, 0);
}
template <typename _D> inline _D _cr_hyp(_D x, int which /*0 sinh 1 cosh 2 tanh*/) {
    _Mp X = _mp_from(x._M_w());
    return _mp_ziv<_D>([&](int L, long* lost, int* sp) {
        int o; _Mp e = _mp_exp(X, L + 2, &o);
        if (o) {
            if (which == 2) return o > 0 ? _mp_u(1) : _mp_neg(_mp_u(1));  // |tanh| rounds as 1 (just below 1: see notes)
            *sp = (which == 0 && o < 0) ? 2 : 1;
            return _Mp();
        }
        _Mp ie = _mp_div(_mp_u(1), e, L + 2);
        _Mp s = _mp_divs(_mp_sub(e, ie, L + 2), 2, L + 2), c = _mp_divs(_mp_add(e, ie, L + 2), 2, L + 2);
        *lost = (which != 1 && !s.zero() && _mp_dexp(s) < 0) ? -_mp_dexp(s) : 0;
        return which == 0 ? s : which == 1 ? c : _mp_div(s, c, L);
    }, 0);
}
template <typename _D> inline _D _cr_cbrt(_D x) {
    _Mp X = _mp_from(x._M_w()); bool ng = X.neg; X.neg = false;
    return _mp_ziv<_D>([&](int L, long*, int*) {
        long ex;
        // y = f^(-1/3) by Newton: y += y (1 - f y^3) / 3; cbrt = f y^2 * 10^e3
        long E = _mp_dexp(X); long e3 = E >= 0 ? E / 3 : -((-E + 2) / 3);
        _Mp f = _mp_shift10(X, -3 * e3, L + 2);  // [1, 1000)
        double fd = _mp_dbl(f, &ex);
        _Mp y = _mp_fromd(1.0 / __builtin_cbrt(fd), 0);
        for (int p = 2; ; p = p * 2 < L + 2 ? p * 2 : L + 2) {
            _Mp e = _mp_sub(_mp_u(1), _mp_mul(f, _mp_mul(y, _mp_mul(y, y, p + 2), p + 2), p + 2), p + 2);
            y = _mp_add(y, _mp_divs(_mp_mul(y, e, p + 2), 3, p + 2), p + 2);
            if (p == L + 2) break;
        }
        _Mp r = _mp_shift10(_mp_mul(f, _mp_mul(y, y, L + 2), L + 1), e3, L + 1);
        if (ng) r = _mp_neg(r);
        return r;
    }, 0);
}
template <typename _D> inline _D _cr_hypot(_D x, _D y) {
    _Mp X = _mp_from(x._M_w()), Y = _mp_from(y._M_w());
    _Mp s = _mp_add(_mp_mul(X, X, 1 << 20), _mp_mul(Y, Y, 1 << 20), 1 << 20);
    if (s.zero()) return _D(0);
    return _mp_ziv<_D>([&](int L, long*, int*) { return _mp_sqrt(s, L); }, 0);
}
template <typename _D> inline _D _cr_pow(_D x, _D y) {
    _Mp X = _mp_from(x._M_w()), Y = _mp_from(y._M_w());
    return _mp_ziv<_D>([&](int L, long*, int* sp) {
        int ex = (int)(_mp_dexp(Y) > 0 ? _mp_dexp(Y) : 0) / 9 + 2;
        int o; _Mp e = _mp_exp(_mp_mul(Y, _mp_log(X, L + ex + 2), L + ex + 2), L, &o);
        if (o) { *sp = o; return _Mp(); }
        return e;
    }, 0);
}
