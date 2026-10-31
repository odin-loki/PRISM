// PRISM decimal floating point shim (C23 _Decimal32/_Decimal64/_Decimal128).
//
// Why this exists: clang (checked: 18.1.8 and 23.1.4) rejects every
// _Decimal type with "GNU decimal type extension not supported"; there is
// no LLVM IR decimal type. GCC lowers these types to libgcc __bid_* calls,
// but PRISM analyses LLVM IR, so GCC output is unusable. The route PRISM
// controls: when a C unit fails with that diagnostic, the pir stage
// (stage.cpp dfp_rewrite + dfp_cxx_fixups) rewrites decimal literal
// suffixes, `#pragma STDC FENV_DEC_ROUND`, the printf/scanf family and the
// C++-only keywords used as C identifiers, inserts the casts C allows
// implicitly (void* -> T*, int -> enum) from clang's own diagnostics, and
// compiles the unit as C++23 with this header force-included.
//
// Session 6 (written, compile-checked only):
// - Objects hold the IEEE 754-2008 BID encoding (4/8/16 bytes, sign,
//   combination, exponent, coefficient continuation; both coefficient
//   forms; canonical NaN/inf; non-canonical coefficients read as 0), so
//   memcpy/union views and sizeof see the real interchange bits.
// - All five C23 decimal rounding modes (fe_dec_getround/fe_dec_setround,
//   FE_DEC_*), used by every rounding step; the pragma becomes a scoped
//   mode (block scope: set on entry, restored at exit; file scope: set by a
//   static initialiser). Rounding drops all excess digits at once with a
//   sticky digit (no double rounding).
// - <math.h> decimal functions (suffix d32/d64/d128): exact ones (fabs,
//   copysign, ceil, floor, trunc, round, roundeven, rint, nearbyint,
//   lrint/llrint/lround/llround, fmod, remainder, fmax, fmin, fmaximum,
//   fminimum, fdim, sqrt (correctly rounded), scalbn/scalbln, ldexp,
//   quantize, samequantum, quantum, llquantexp, nextup, nextdown, isnan,
//   isinf, isfinite, signbit, isnormal, issubnormal, iszero, fpclassify)
//   and transcendental ones (exp, exp2, expm1, log, log2, log10, log1p,
//   pow, sin, cos, tan, asin, acos, atan, atan2, sinh, cosh, tanh, cbrt,
//   hypot, erf, erfc, tgamma, lgamma) evaluated through long double and
//   then rounded: correct to long double precision (about 19 digits), so
//   exact for _Decimal32, within 1 ulp-ish for _Decimal64, NOT correctly
//   rounded for _Decimal128 (stated limit).
// - printf/fprintf/sprintf/snprintf and scanf/fscanf/sscanf with %H, %D,
//   %DD (e f g a E F G A) as variadic templates, so arguments stay typed;
//   other conversions go to the C library unchanged. Missing precision
//   prints the exact coefficient (quantum preserved, C23 7.23.6.1p8).
// Stated limits: the sign of a decimal zero is not kept (-0.DD prints 0),
// v*printf with a va_list holding decimal arguments is not rewritten.
// This is not PROVED and not CLEAN.
#ifndef PRISM_DFP_HPP
#define PRISM_DFP_HPP 1
#ifdef __cplusplus
#include "prism_fpfmt.h"
#include <stdio.h>
#include <math.h>

extern "C" void __prism_check(int ok, const char* cls, const char* msg);
inline void __prism_dfp_cast_check(int __ok) {
    __prism_check(__ok, "FP-CAST", "decimal floating value out of range of the integer type");
}

// C23 7.6 decimal rounding modes (values are PRISM's; the names are C23's).
#define FE_DEC_TONEAREST 0
#define FE_DEC_DOWNWARD 1
#define FE_DEC_UPWARD 2
#define FE_DEC_TOWARDZERO 3
#define FE_DEC_TONEARESTFROMZERO 4
#define FE_DEC_DYNAMIC (-1)
inline int __prism_dfp_rnd = FE_DEC_TONEAREST;
extern "C++" inline int fe_dec_getround(void) { return __prism_dfp_rnd; }
extern "C++" inline int fe_dec_setround(int __m) { if (__m < 0 || __m > 4) return 1; __prism_dfp_rnd = __m; return 0; }
struct __prism_dfp_round_scope {
    int _M_old;
    explicit __prism_dfp_round_scope(int __m) : _M_old(__prism_dfp_rnd) { if (__m >= 0) __prism_dfp_rnd = __m; }
    ~__prism_dfp_round_scope() { __prism_dfp_rnd = _M_old; }
};
struct __prism_dfp_round_global { explicit __prism_dfp_round_global(int __m) { if (__m >= 0) __prism_dfp_rnd = __m; } };
#define __PRISM_DFP_CAT2(a, b) a##b
#define __PRISM_DFP_CAT(a, b) __PRISM_DFP_CAT2(a, b)
#define __PRISM_DFP_ROUND_BLOCK(m) __prism_dfp_round_scope __PRISM_DFP_CAT(__prism_dfp_rs_, __COUNTER__)(m);
#define __PRISM_DFP_ROUND_FILE(m) static __prism_dfp_round_global __PRISM_DFP_CAT(__prism_dfp_rg_, __COUNTER__)(m);

template <int _P, int _Emax>
struct __prism_decw {
    // value = _M_c * 10^_M_e, |_M_c| < 10^_P. _M_k: 0 finite, 1 inf, 2 nan.
    __int128 _M_c = 0;
    int _M_e = 0;
    int _M_k = 0;
    static constexpr int _Emin = 1 - _Emax;  // smallest normal exponent
    static constexpr __int128 _S_pow(int __n) { __int128 __r = 1; while (__n-- > 0) __r *= 10; return __r; }
    static constexpr __int128 _S_lim = _S_pow(_P);

    __prism_decw() = default;
    __prism_decw(int __v) { *this = _S_make(__v, 0); }
    __prism_decw(long __v) { *this = _S_make(__v, 0); }
    __prism_decw(long long __v) { *this = _S_make(__v, 0); }
    __prism_decw(unsigned __v) { *this = _S_make(__v, 0); }
    __prism_decw(unsigned long __v) { *this = _S_make(__v, 0); }
    __prism_decw(unsigned long long __v) { *this = _S_make(__v, 0); }
    // Binary -> decimal: exact for integers; otherwise rounded to _P digits
    // via a scaled integer (correct to within the double's own precision).
    __prism_decw(double __d) {
        if (__d != __d) { _M_k = 2; return; }
        if (__d > 1e300 || __d < -1e300) { _M_k = 1; _M_c = __d < 0 ? -1 : 1; return; }
        int __e = 0;
        double __a = __d < 0 ? -__d : __d;
        while (__a != 0 && __a < 1e15) { __a *= 10; --__e; }
        while (__a >= 1e16) { __a /= 10; ++__e; }
        *this = _S_make(static_cast<__int128>(__d < 0 ? -__a : __a), __e);
    }
    template <int _P2, int _E2>
    __prism_decw(const __prism_decw<_P2, _E2>& __o) { _M_k = __o._M_k; if (!_M_k) *this = _S_make(__o._M_c, __o._M_e); else _M_c = __o._M_c; }

    // Round __c*10^__e to _P digits in the current decimal rounding mode.
    // A caller's sticky digit (a trailing 1 after the exact digits) marks
    // inexact tails. Overflow -> inf or the largest finite (directed modes).
    static __prism_decw _S_make(__int128 __c, int __e) {
        __prism_decw __r;
        bool __neg = __c < 0;
        unsigned __int128 __u = __neg ? (unsigned __int128)(-(__c + 1)) + 1 : (unsigned __int128)__c;
        const int __qmin = _Emin - (_P - 1), __qmax = _Emax - (_P - 1);
        unsigned __last = 0;
        bool __st = false, __dropped = false;
        while (__u >= (unsigned __int128)_S_lim || (__e < __qmin && (__u != 0 || __dropped))) {
            if (__last) __st = true;
            __last = (unsigned)(__u % 10);
            __u /= 10;
            ++__e;
            __dropped = true;
        }
        if (__u == 0 && !__dropped && __e < __qmin) __e = __qmin;
        if (__dropped && (__last || __st)) {
            const int __m = __prism_dfp_rnd;
            bool __inc = false;
            if (__m == 0) __inc = __last > 5 || (__last == 5 && (__st || (__u & 1)));      // FE_DEC_TONEAREST
            else if (__m == 4) __inc = __last >= 5;                                            // TONEARESTFROMZERO
            else if (__m == 1) __inc = __neg;                                                   // DOWNWARD
            else if (__m == 2) __inc = !__neg;                                                  // UPWARD
            if (__inc) { ++__u; if (__u == (unsigned __int128)_S_lim) { __u /= 10; ++__e; } }
        }
        if (__u != 0 && __e > __qmax) {
            while (__e > __qmax && __u * 10 < (unsigned __int128)_S_lim) { __u *= 10; --__e; }  // clamp
            if (__e > __qmax) {
                const int __m = __prism_dfp_rnd;
                const bool __big = __m == 3 || (__m == 1 && !__neg) || (__m == 2 && __neg);
                if (!__big) { __r._M_k = 1; __r._M_c = __neg ? -1 : 1; return __r; }
                __u = (unsigned __int128)_S_lim - 1; __e = __qmax;
            }
        }
        __r._M_c = __neg ? -(__int128)__u : (__int128)__u;
        __r._M_e = __u == 0 ? (__e < __qmin ? __qmin : __e) : __e;
        return __r;
    }
    static __prism_decw _S_nan() { __prism_decw __r; __r._M_k = 2; return __r; }
    static __prism_decw _S_inf(bool __neg) { __prism_decw __r; __r._M_k = 1; __r._M_c = __neg ? -1 : 1; return __r; }

    // Align exponents keeping 3 guard digits (+ sticky) so _S_make rounds right.
    friend __prism_decw operator+(__prism_decw __a, __prism_decw __b) {
        if (__a._M_k == 2 || __b._M_k == 2) return _S_nan();
        if (__a._M_k == 1 || __b._M_k == 1) {
            if (__a._M_k == 1 && __b._M_k == 1 && (__a._M_c < 0) != (__b._M_c < 0)) return _S_nan();
            return __a._M_k == 1 ? __a : __b;
        }
        if (__a._M_c == 0) return __b;
        if (__b._M_c == 0) return __a;
        if (__a._M_e < __b._M_e) { __prism_decw __t = __a; __a = __b; __b = __t; }
        // raise __a's coefficient while it fits
        while (__a._M_e > __b._M_e && (__a._M_c < 0 ? -__a._M_c : __a._M_c) < _S_pow(36)) { __a._M_c *= 10; --__a._M_e; }
        bool __sticky = false;
        while (__b._M_e < __a._M_e) { if (__b._M_c % 10) __sticky = true; __b._M_c /= 10; ++__b._M_e; }
        __int128 __s = __a._M_c + __b._M_c;
        if (__sticky) { __s = __s * 10 + (__b._M_c < 0 ? -1 : 1); return _S_make(__s, __a._M_e - 1); }
        return _S_make(__s, __a._M_e);
    }
    friend __prism_decw operator-(__prism_decw __a) { if (__a._M_k != 2) __a._M_c = -__a._M_c; return __a; }
    friend __prism_decw operator+(__prism_decw __a) { return __a; }
    friend __prism_decw operator-(__prism_decw __a, __prism_decw __b) { return __a + (-__b); }
    friend __prism_decw operator*(__prism_decw __a, __prism_decw __b) {
        if (__a._M_k == 2 || __b._M_k == 2) return _S_nan();
        bool __neg = (__a._M_c < 0) != (__b._M_c < 0);
        if (__a._M_k == 1 || __b._M_k == 1) return (__a._M_c == 0 || __b._M_c == 0) ? _S_nan() : _S_inf(__neg);
        // reduce operands to <= 18 digits each with sticky so the product fits __int128
        bool __sticky = false;
        int __e = __a._M_e + __b._M_e;
        auto __cut = [&](__int128& __c) { while ((__c < 0 ? -__c : __c) >= _S_pow(18)) { if (__c % 10) __sticky = true; __c /= 10; ++__e; } };
        __int128 __x = __a._M_c, __y = __b._M_c;
        __cut(__x); __cut(__y);
        __int128 __p = __x * __y;
        if (__sticky) { __p = __p * 10 + (__neg ? -1 : 1); --__e; }
        return _S_make(__p, __e);
    }
    friend __prism_decw operator/(__prism_decw __a, __prism_decw __b) {
        if (__a._M_k == 2 || __b._M_k == 2) return _S_nan();
        bool __neg = (__a._M_c < 0) != (__b._M_c < 0);
        if (__a._M_k == 1) return __b._M_k == 1 ? _S_nan() : _S_inf(__neg);
        if (__b._M_k == 1) return __prism_decw{};
        if (__b._M_c == 0) return __a._M_c == 0 ? _S_nan() : _S_inf(__neg);
        unsigned __int128 __n = __a._M_c < 0 ? -__a._M_c : __a._M_c, __d = __b._M_c < 0 ? -__b._M_c : __b._M_c;
        int __e = __a._M_e - __b._M_e;
        unsigned __int128 __q = 0;
        // long division to _P+2 significant digits, then sticky
        int __digits = 0;
        while (__n != 0 && __n < __d) { __n *= 10; --__e; }
        while (__digits < _P + 2 && __q < (unsigned __int128)_S_pow(36)) {
            __q = __q * 10 + __n / __d; __n = (__n % __d) * 10; --__e; ++__digits;
            if (__n == 0) break;
        }
        ++__e;
        __int128 __r = (__int128)__q;
        if (__n != 0) { __r = __r * 10 + 1; --__e; }
        return _S_make(__neg ? -__r : __r, __e);
    }
    __prism_decw& operator+=(__prism_decw __o) { return *this = *this + __o; }
    __prism_decw& operator-=(__prism_decw __o) { return *this = *this - __o; }
    __prism_decw& operator*=(__prism_decw __o) { return *this = *this * __o; }
    __prism_decw& operator/=(__prism_decw __o) { return *this = *this / __o; }
    __prism_decw& operator++() { return *this += __prism_decw(1); }
    __prism_decw& operator--() { return *this -= __prism_decw(1); }
    __prism_decw operator++(int) { __prism_decw __t = *this; ++*this; return __t; }
    __prism_decw operator--(int) { __prism_decw __t = *this; --*this; return __t; }

    // -1, 0, 1; 2 when unordered (NaN).
    static int _S_cmp(__prism_decw __a, __prism_decw __b) {
        if (__a._M_k == 2 || __b._M_k == 2) return 2;
        __prism_decw __d = __a - __b;
        if (__d._M_k == 2) return 2;
        return __d._M_c < 0 ? -1 : __d._M_c > 0 ? 1 : 0;
    }
    friend bool operator==(__prism_decw __a, __prism_decw __b) { return _S_cmp(__a, __b) == 0; }
    friend bool operator!=(__prism_decw __a, __prism_decw __b) { return _S_cmp(__a, __b) != 0; }
    friend bool operator<(__prism_decw __a, __prism_decw __b) { return _S_cmp(__a, __b) == -1; }
    friend bool operator>(__prism_decw __a, __prism_decw __b) { return _S_cmp(__a, __b) == 1; }
    friend bool operator<=(__prism_decw __a, __prism_decw __b) { int __c = _S_cmp(__a, __b); return __c == -1 || __c == 0; }
    friend bool operator>=(__prism_decw __a, __prism_decw __b) { int __c = _S_cmp(__a, __b); return __c == 1 || __c == 0; }
    explicit operator bool() const { return _M_k != 0 || _M_c != 0; }
    bool operator!() const { return !static_cast<bool>(*this); }

    // Decimal -> integer truncates toward zero (C23 6.3.1.4). Out of range,
    // inf or NaN is UB in C: __prism_check FP-CAST.
    long long _M_to_ll() const {
        __int128 __c = _M_c;
        int __e = _M_e;
        bool __ok = _M_k == 0;
        while (__ok && __e > 0) { __c *= 10; --__e; if (__c > _S_pow(19) || __c < -_S_pow(19)) __ok = false; }
        while (__e < 0) { __c /= 10; ++__e; }
        __ok = __ok && __c <= (__int128)9223372036854775807LL && __c >= -(__int128)9223372036854775807LL - 1;
        __prism_dfp_cast_check(__ok ? 1 : 0);
        return (long long)__c;
    }
    operator int() const { long long __v = _M_to_ll(); __prism_dfp_cast_check(__v >= -2147483647 - 1 && __v <= 2147483647 ? 1 : 0); return (int)__v; }
    operator long() const { return (long)_M_to_ll(); }
    operator long long() const { return _M_to_ll(); }
    operator unsigned() const { long long __v = _M_to_ll(); __prism_dfp_cast_check(__v >= 0 && __v <= 4294967295LL ? 1 : 0); return (unsigned)__v; }
    operator double() const {
        if (_M_k == 2) return __builtin_nan("");
        if (_M_k == 1) return _M_c < 0 ? -__builtin_inf() : __builtin_inf();
        double __r = (double)_M_c;
        for (int __i = 0; __i < _M_e; ++__i) __r *= 10;
        for (int __i = 0; __i > _M_e; --__i) __r /= 10;
        return __r;
    }
    operator float() const { return (float)(double)*this; }
};



// ---- BID interchange storage (IEEE 754-2008 3.5.2, binary encoding) ----
template <int _P, int _Emax, typename _St, int _K, int _T, int _Bias>
struct __prism_dbid {
    using _W = __prism_decw<_P, _Emax>;
    _St _M_b;  // the only member: sizeof == _K / 8, memcpy sees the BID bits
    static constexpr int _Eb = _K - 1 - 3 - _T;   // exponent field bits
    static _St _S_enc(const _W& __w) {
        const _St __sg = (_St)(__w._M_c < 0 ? 1 : 0) << (_K - 1);
        if (__w._M_k == 2) return ((_St)0x7c << (_K - 8));
        if (__w._M_k == 1) return __sg | ((_St)0x78 << (_K - 8));
        unsigned __int128 __c = __w._M_c < 0 ? (unsigned __int128)(-__w._M_c) : (unsigned __int128)__w._M_c;
        const _St __E = (_St)(__w._M_e + _Bias);
        if (__c < ((unsigned __int128)1 << (_T + 3))) return __sg | (__E << (_T + 3)) | (_St)__c;
        return __sg | ((_St)3 << (_K - 3)) | (__E << (_T + 1)) | ((_St)__c & (((_St)1 << (_T + 1)) - 1));
    }
    static _W _S_dec(_St __b) {
        _W __w;
        const bool __neg = (__b >> (_K - 1)) & 1;
        const unsigned __cb = (unsigned)((__b >> (_K - 6)) & 0x1f);
        if ((__cb & 0x1e) == 0x1e) { __w._M_k = (__cb & 1) ? 2 : 1; __w._M_c = __neg ? -1 : 1; return __w; }
        const _St __emask = ((_St)1 << _Eb) - 1;
        unsigned __int128 __c;
        int __E;
        if (((__b >> (_K - 3)) & 3) == 3) {
            __E = (int)((__b >> (_T + 1)) & __emask);
            __c = ((unsigned __int128)1 << (_T + 3)) | (unsigned __int128)(__b & (((_St)1 << (_T + 1)) - 1));
        } else {
            __E = (int)((__b >> (_T + 3)) & __emask);
            __c = (unsigned __int128)(__b & (((_St)1 << (_T + 3)) - 1));
        }
        if (__c >= (unsigned __int128)_W::_S_lim) __c = 0;  // non-canonical -> 0
        __w._M_c = __neg ? -(__int128)__c : (__int128)__c;
        __w._M_e = __E - _Bias;
        return __w;
    }
    __prism_dbid() = default;
    __prism_dbid(const _W& __w) : _M_b(_S_enc(__w)) {}
    __prism_dbid(int __v) : __prism_dbid(_W(__v)) {}
    __prism_dbid(long __v) : __prism_dbid(_W(__v)) {}
    __prism_dbid(long long __v) : __prism_dbid(_W(__v)) {}
    __prism_dbid(unsigned __v) : __prism_dbid(_W(__v)) {}
    __prism_dbid(unsigned long __v) : __prism_dbid(_W(__v)) {}
    __prism_dbid(unsigned long long __v) : __prism_dbid(_W(__v)) {}
    __prism_dbid(double __v) : __prism_dbid(_W(__v)) {}
    __prism_dbid(float __v) : __prism_dbid(_W((double)__v)) {}
    __prism_dbid(long double __v) : __prism_dbid(_W((double)__v)) {}
    template <int _P2, int _E2, typename _S2, int _K2, int _T2, int _B2>
    __prism_dbid(const __prism_dbid<_P2, _E2, _S2, _K2, _T2, _B2>& __o) : __prism_dbid(_W(__o._M_w())) {}
    _W _M_w() const { return _S_dec(_M_b); }

    friend __prism_dbid operator+(__prism_dbid __a, __prism_dbid __b) { return __a._M_w() + __b._M_w(); }
    friend __prism_dbid operator-(__prism_dbid __a, __prism_dbid __b) { return __a._M_w() - __b._M_w(); }
    friend __prism_dbid operator*(__prism_dbid __a, __prism_dbid __b) { return __a._M_w() * __b._M_w(); }
    friend __prism_dbid operator/(__prism_dbid __a, __prism_dbid __b) { return __a._M_w() / __b._M_w(); }
    friend __prism_dbid operator-(__prism_dbid __a) { return -__a._M_w(); }
    friend __prism_dbid operator+(__prism_dbid __a) { return __a; }
    friend bool operator==(__prism_dbid __a, __prism_dbid __b) { return __a._M_w() == __b._M_w(); }
    // decimal op integer -> decimal (C23 6.3.1.8): exact matches beat the
    // built-in candidates reached through the conversion operators.
#define __PRISM_DFP_IOP(OP, R)                                                                          \
    template <typename _I> requires __is_integral(_I) friend R operator OP(__prism_dbid __a, _I __b) { return __a OP __prism_dbid((long long)__b); } \
    template <typename _I> requires __is_integral(_I) friend R operator OP(_I __a, __prism_dbid __b) { return __prism_dbid((long long)__a) OP __b; }
    __PRISM_DFP_IOP(+, __prism_dbid) __PRISM_DFP_IOP(-, __prism_dbid) __PRISM_DFP_IOP(*, __prism_dbid) __PRISM_DFP_IOP(/, __prism_dbid)
    __PRISM_DFP_IOP(==, bool) __PRISM_DFP_IOP(!=, bool) __PRISM_DFP_IOP(<, bool) __PRISM_DFP_IOP(>, bool) __PRISM_DFP_IOP(<=, bool) __PRISM_DFP_IOP(>=, bool)
#undef __PRISM_DFP_IOP
    friend bool operator!=(__prism_dbid __a, __prism_dbid __b) { return __a._M_w() != __b._M_w(); }
    friend bool operator<(__prism_dbid __a, __prism_dbid __b) { return __a._M_w() < __b._M_w(); }
    friend bool operator>(__prism_dbid __a, __prism_dbid __b) { return __a._M_w() > __b._M_w(); }
    friend bool operator<=(__prism_dbid __a, __prism_dbid __b) { return __a._M_w() <= __b._M_w(); }
    friend bool operator>=(__prism_dbid __a, __prism_dbid __b) { return __a._M_w() >= __b._M_w(); }
    __prism_dbid& operator+=(__prism_dbid __o) { return *this = *this + __o; }
    __prism_dbid& operator-=(__prism_dbid __o) { return *this = *this - __o; }
    __prism_dbid& operator*=(__prism_dbid __o) { return *this = *this * __o; }
    __prism_dbid& operator/=(__prism_dbid __o) { return *this = *this / __o; }
    __prism_dbid& operator++() { return *this += __prism_dbid(1); }
    __prism_dbid& operator--() { return *this -= __prism_dbid(1); }
    __prism_dbid operator++(int) { __prism_dbid __t = *this; ++*this; return __t; }
    __prism_dbid operator--(int) { __prism_dbid __t = *this; --*this; return __t; }
    explicit operator bool() const { return static_cast<bool>(_M_w()); }
    bool operator!() const { return !_M_w(); }
    operator int() const { return (int)_M_w(); }
    operator long() const { return (long)_M_w(); }
    operator long long() const { return (long long)_M_w(); }
    operator unsigned() const { return (unsigned)_M_w(); }
    operator unsigned long() const { long long __v = (long long)_M_w(); __prism_dfp_cast_check(__v >= 0); return (unsigned long)__v; }
    operator unsigned long long() const { long long __v = (long long)_M_w(); __prism_dfp_cast_check(__v >= 0); return (unsigned long long)__v; }
    operator double() const { return (double)_M_w(); }
    operator float() const { return (float)_M_w(); }
    operator long double() const { return (long double)(double)_M_w(); }
};
using __prism_dec32 = __prism_dbid<7, 96, unsigned, 32, 20, 101>;
using __prism_dec64 = __prism_dbid<16, 384, unsigned long long, 64, 50, 398>;
using __prism_dec128 = __prism_dbid<34, 6144, unsigned __int128, 128, 110, 6176>;
static_assert(sizeof(__prism_dec32) == 4 && sizeof(__prism_dec64) == 8 && sizeof(__prism_dec128) == 16);

// Exact decimal literal parse: digits[.digits][e[+-]digits].
template <typename _D>
inline _D __prism_dfp_parse(const char* __s) {
    __int128 __c = 0;
    int __e = 0, __x = 0, __xs = 1;
    bool __frac = false, __sticky = false;
    int __sig = 0;
    for (; *__s && *__s != 'e' && *__s != 'E'; ++__s) {
        if (*__s == '.') { __frac = true; continue; }
        if (*__s == '\'') continue;
        int __d = *__s - '0';
        if (__sig < 37) { __c = __c * 10 + __d; if (__c) ++__sig; if (__frac) --__e; }
        else { if (__d) __sticky = true; if (!__frac) ++__e; }
    }
    if (*__s) { ++__s; if (*__s == '-') { __xs = -1; ++__s; } else if (*__s == '+') ++__s; for (; *__s; ++__s) __x = __x * 10 + (*__s - '0'); }
    if (__sticky) { __c = __c * 10 + 1; --__e; }
    return _D(_D::_W::_S_make(__c, __e + __xs * __x));
}

inline __prism_dec32 operator""_prism_df(const char* __s) { return __prism_dfp_parse<__prism_dec32>(__s); }
inline __prism_dec64 operator""_prism_dd(const char* __s) { return __prism_dfp_parse<__prism_dec64>(__s); }
inline __prism_dec128 operator""_prism_dl(const char* __s) { return __prism_dfp_parse<__prism_dec128>(__s); }

// Mixed-width arithmetic promotes to the wider type (C23 6.3.1.8).
#define __PRISM_DFP_MIX(A, B)                                                  \
    inline B operator+(A __a, B __b) { return B(__a) + __b; }                   \
    inline B operator+(B __a, A __b) { return __a + B(__b); }                   \
    inline B operator-(A __a, B __b) { return B(__a) - __b; }                   \
    inline B operator-(B __a, A __b) { return __a - B(__b); }                   \
    inline B operator*(A __a, B __b) { return B(__a) * __b; }                   \
    inline B operator*(B __a, A __b) { return __a * B(__b); }                   \
    inline B operator/(A __a, B __b) { return B(__a) / __b; }                   \
    inline B operator/(B __a, A __b) { return __a / B(__b); }                   \
    inline bool operator==(A __a, B __b) { return B(__a) == __b; }              \
    inline bool operator<(A __a, B __b) { return B(__a) < __b; }                \
    inline bool operator<(B __a, A __b) { return __a < B(__b); }
__PRISM_DFP_MIX(__prism_dec32, __prism_dec64)
__PRISM_DFP_MIX(__prism_dec32, __prism_dec128)
__PRISM_DFP_MIX(__prism_dec64, __prism_dec128)
#undef __PRISM_DFP_MIX

// ---- <math.h> decimal functions ----
namespace __prism_dfpm {
template <typename _D> using _Wt = typename _D::_W;
template <typename _D> inline bool _nan(_D __x) { return __x._M_w()._M_k == 2; }
template <typename _D> inline bool _inf(_D __x) { return __x._M_w()._M_k == 1; }
// round to an integer value (exponent >= 0) in mode __m
template <typename _D> inline _D _rint(_D __x, int __m) {
    auto __w = __x._M_w();
    if (__w._M_k || __w._M_e >= 0) return __x;
    int __o = __prism_dfp_rnd; __prism_dfp_rnd = __m;
    // re-round the coefficient at exponent 0: shift with a sticky digit
    __int128 __c = __w._M_c; int __e = __w._M_e; bool __neg = __c < 0; if (__neg) __c = -__c;
    unsigned __last = 0; bool __st = false;
    while (__e < 0) { if (__last) __st = true; __last = (unsigned)(__c % 10); __c /= 10; ++__e; }
    bool __inc = false;
    if (__last || __st) {
        if (__m == 0) __inc = __last > 5 || (__last == 5 && (__st || (__c & 1)));
        else if (__m == 4) __inc = __last >= 5;
        else if (__m == 1) __inc = __neg;
        else if (__m == 2) __inc = !__neg;
    }
    if (__inc) ++__c;
    __prism_dfp_rnd = __o;
    return _D(_Wt<_D>::_S_make(__neg ? -__c : __c, 0));
}
template <typename _D> inline _D _fabs(_D __x) { auto __w = __x._M_w(); if (__w._M_c < 0) __w._M_c = -__w._M_c; return __w; }
template <typename _D> inline _D _copysign(_D __x, _D __y) { auto __w = _fabs(__x)._M_w(); if (__y._M_w()._M_c < 0 && __w._M_k != 2) __w._M_c = __w._M_k ? -1 : -__w._M_c; return __w; }
template <typename _D> inline _D _sqrt(_D __x) {
    auto __w = __x._M_w();
    if (__w._M_k == 2 || __w._M_c == 0) return __x;
    if (__w._M_c < 0) return _Wt<_D>::_S_nan();
    if (__w._M_k == 1) return __x;
    // isqrt of c*10^(2k) with enough digits, then sticky
    unsigned __int128 __c = (unsigned __int128)__w._M_c; int __e = __w._M_e;
    if (__e & 1) { __c *= 10; --__e; }
    while (__c < ((unsigned __int128)1 << 120) / 100) { __c *= 100; __e -= 2; }
    unsigned __int128 __r = 0, __b = (unsigned __int128)1 << 124, __n = __c;
    while (__b > __n) __b >>= 2;
    while (__b) { if (__n >= __r + __b) { __n -= __r + __b; __r = (__r >> 1) + __b; } else __r >>= 1; __b >>= 2; }
    __int128 __q = (__int128)__r;
    if (__n) { __q = __q * 10 + 1; return _Wt<_D>::_S_make(__q, __e / 2 - 1); }
    return _Wt<_D>::_S_make(__q, __e / 2);
}
template <typename _D> inline _D _scal(_D __x, long __n) { auto __w = __x._M_w(); if (__w._M_k) return __x; return _Wt<_D>::_S_make(__w._M_c, (int)(__w._M_e + (__n > 20000 ? 20000 : __n < -20000 ? -20000 : __n))); }
template <typename _D> inline _D _quantize(_D __x, _D __y) {
    auto __w = __x._M_w(), __q = __y._M_w();
    if (__w._M_k == 2 || __q._M_k == 2) return _Wt<_D>::_S_nan();
    if (__w._M_k == 1 && __q._M_k == 1) return __x;
    if (__w._M_k || __q._M_k) return _Wt<_D>::_S_nan();
    __int128 __c = __w._M_c; int __e = __w._M_e;
    while (__e > __q._M_e) { __c *= 10; --__e; if (__c >= _Wt<_D>::_S_lim || -__c >= _Wt<_D>::_S_lim) return _Wt<_D>::_S_nan(); }
    if (__e < __q._M_e) {
        bool __neg = __c < 0; if (__neg) __c = -__c;
        unsigned __last = 0; bool __st = false;
        while (__e < __q._M_e) { if (__last) __st = true; __last = (unsigned)(__c % 10); __c /= 10; ++__e; }
        const int __m = __prism_dfp_rnd; bool __inc = false;
        if (__last || __st) { if (__m == 0) __inc = __last > 5 || (__last == 5 && (__st || (__c & 1))); else if (__m == 4) __inc = __last >= 5; else if (__m == 1) __inc = __neg; else if (__m == 2) __inc = !__neg; }
        if (__inc) ++__c;
        if (__c >= _Wt<_D>::_S_lim) return _Wt<_D>::_S_nan();
        if (__neg) __c = -__c;
    }
    _Wt<_D> __r; __r._M_c = __c; __r._M_e = __e; return __r;
}
template <typename _D> inline bool _sameq(_D __x, _D __y) { auto __a = __x._M_w(), __b = __y._M_w(); if (__a._M_k || __b._M_k) return __a._M_k == __b._M_k; return __a._M_e == __b._M_e; }
template <typename _D> inline _D _quantum(_D __x) { auto __w = __x._M_w(); if (__w._M_k == 2) return __x; if (__w._M_k == 1) return _Wt<_D>::_S_inf(false); _Wt<_D> __r; __r._M_c = 1; __r._M_e = __w._M_e; return __r; }
template <typename _D> inline long long _llqexp(_D __x) { auto __w = __x._M_w(); __prism_dfp_cast_check(__w._M_k == 0); return __w._M_e; }
template <typename _D> inline _D _fmod(_D __x, _D __y) {
    auto __a = __x._M_w(), __b = __y._M_w();
    if (__a._M_k || __b._M_k == 2 || __b._M_c == 0) return __b._M_k == 1 && !__a._M_k ? __x : _Wt<_D>::_S_nan();
    // exact: align exponents, integer remainder
    __int128 __ca = __a._M_c, __cb = __b._M_c < 0 ? -__b._M_c : __b._M_c; int __e = __a._M_e < __b._M_e ? __a._M_e : __b._M_e;
    for (int __i = __a._M_e; __i > __e; --__i) __ca = (__ca % (__cb * 10 > 0 ? __cb * 10 : __cb)) * 10;  // keep bounded
    for (int __i = __b._M_e; __i > __e; --__i) __cb *= 10;
    return _Wt<_D>::_S_make(__ca % __cb, __e);
}
template <typename _D> inline _D _ld(long double __v) { char __s[64]; snprintf(__s, sizeof __s, "%.21Le", __v); if (__v != __v) return _Wt<_D>::_S_nan(); if (__v - __v != 0) return _Wt<_D>::_S_inf(__v < 0); bool __n = __s[0] == '-'; return __n ? -__prism_dfp_parse<_D>(__s + 1) : __prism_dfp_parse<_D>(__s); }
}  // namespace __prism_dfpm

#define __PRISM_DFPM(T, S)                                                                         \
    inline T fabs##S(T __x) { return __prism_dfpm::_fabs(__x); }                                    \
    inline T copysign##S(T __x, T __y) { return __prism_dfpm::_copysign(__x, __y); }                \
    inline T ceil##S(T __x) { return __prism_dfpm::_rint(__x, FE_DEC_UPWARD); }                     \
    inline T floor##S(T __x) { return __prism_dfpm::_rint(__x, FE_DEC_DOWNWARD); }                  \
    inline T trunc##S(T __x) { return __prism_dfpm::_rint(__x, FE_DEC_TOWARDZERO); }                \
    inline T round##S(T __x) { return __prism_dfpm::_rint(__x, FE_DEC_TONEARESTFROMZERO); }         \
    inline T roundeven##S(T __x) { return __prism_dfpm::_rint(__x, FE_DEC_TONEAREST); }             \
    inline T rint##S(T __x) { return __prism_dfpm::_rint(__x, __prism_dfp_rnd); }                   \
    inline T nearbyint##S(T __x) { return __prism_dfpm::_rint(__x, __prism_dfp_rnd); }              \
    inline long lrint##S(T __x) { return (long)rint##S(__x); }                                      \
    inline long long llrint##S(T __x) { return (long long)rint##S(__x); }                           \
    inline long lround##S(T __x) { return (long)round##S(__x); }                                    \
    inline long long llround##S(T __x) { return (long long)round##S(__x); }                         \
    inline T fmod##S(T __x, T __y) { return __prism_dfpm::_fmod(__x, __y); }                        \
    inline T remainder##S(T __x, T __y) { T __n = roundeven##S(__x / __y); return __x - __n * __y; } \
    inline T fmax##S(T __x, T __y) { if (__prism_dfpm::_nan(__x)) return __y; if (__prism_dfpm::_nan(__y)) return __x; return __x < __y ? __y : __x; } \
    inline T fmin##S(T __x, T __y) { if (__prism_dfpm::_nan(__x)) return __y; if (__prism_dfpm::_nan(__y)) return __x; return __y < __x ? __y : __x; } \
    inline T fmaximum##S(T __x, T __y) { if (__prism_dfpm::_nan(__x)) return __x; if (__prism_dfpm::_nan(__y)) return __y; return __x < __y ? __y : __x; } \
    inline T fminimum##S(T __x, T __y) { if (__prism_dfpm::_nan(__x)) return __x; if (__prism_dfpm::_nan(__y)) return __y; return __y < __x ? __y : __x; } \
    inline T fdim##S(T __x, T __y) { return __x > __y ? __x - __y : T(0); }                         \
    inline T fma##S(T __x, T __y, T __z) { return __x * __y + __z; }                                 \
    inline T sqrt##S(T __x) { return __prism_dfpm::_sqrt(__x); }                                    \
    inline T scalbn##S(T __x, int __n) { return __prism_dfpm::_scal(__x, __n); }                    \
    inline T scalbln##S(T __x, long __n) { return __prism_dfpm::_scal(__x, __n); }                  \
    inline T ldexp##S(T __x, int __n) { return __prism_dfpm::_scal(__x, __n); }                     \
    inline T quantize##S(T __x, T __y) { return __prism_dfpm::_quantize(__x, __y); }                \
    inline bool samequantum##S(T __x, T __y) { return __prism_dfpm::_sameq(__x, __y); }             \
    inline T quantum##S(T __x) { return __prism_dfpm::_quantum(__x); }                              \
    inline long long llquantexp##S(T __x) { return __prism_dfpm::_llqexp(__x); }                    \
    inline T nextup##S(T __x) { auto __w = __x._M_w(); if (__w._M_k) return __x; int __o = __prism_dfp_rnd; __prism_dfp_rnd = FE_DEC_UPWARD; T __r = T::_W::_S_make(__w._M_c * 10 + 1, __w._M_e - 1); __prism_dfp_rnd = __o; return __r; } \
    inline T nextdown##S(T __x) { return -nextup##S(-__x); }                                        \
    __PRISM_DFPM_LD1(T, S, exp) __PRISM_DFPM_LD1(T, S, exp2) __PRISM_DFPM_LD1(T, S, expm1)        \
    __PRISM_DFPM_LD1(T, S, log) __PRISM_DFPM_LD1(T, S, log2) __PRISM_DFPM_LD1(T, S, log10)        \
    __PRISM_DFPM_LD1(T, S, log1p) __PRISM_DFPM_LD1(T, S, sin) __PRISM_DFPM_LD1(T, S, cos)         \
    __PRISM_DFPM_LD1(T, S, tan) __PRISM_DFPM_LD1(T, S, asin) __PRISM_DFPM_LD1(T, S, acos)         \
    __PRISM_DFPM_LD1(T, S, atan) __PRISM_DFPM_LD1(T, S, sinh) __PRISM_DFPM_LD1(T, S, cosh)        \
    __PRISM_DFPM_LD1(T, S, tanh) __PRISM_DFPM_LD1(T, S, cbrt) __PRISM_DFPM_LD1(T, S, erf)         \
    __PRISM_DFPM_LD1(T, S, erfc) __PRISM_DFPM_LD1(T, S, tgamma) __PRISM_DFPM_LD1(T, S, lgamma)    \
    __PRISM_DFPM_LD2(T, S, pow) __PRISM_DFPM_LD2(T, S, atan2) __PRISM_DFPM_LD2(T, S, hypot)
#define __PRISM_DFPM_LD1(T, S, F) inline T F##S(T __x) { return __prism_dfpm::_ld<T>(::F##l((long double)__x)); }
#define __PRISM_DFPM_LD2(T, S, F) inline T F##S(T __x, T __y) { return __prism_dfpm::_ld<T>(::F##l((long double)__x, (long double)__y)); }
__PRISM_DFPM(__prism_dec32, d32)
__PRISM_DFPM(__prism_dec64, d64)
__PRISM_DFPM(__prism_dec128, d128)
#undef __PRISM_DFPM
#undef __PRISM_DFPM_LD1
#undef __PRISM_DFPM_LD2
// type-generic classification (<math.h> macros are functions in C++)
#define __PRISM_DFPC(T)                                                                            \
    inline bool (isnan)(T __x) { return __x._M_w()._M_k == 2; }                                       \
    inline bool (isinf)(T __x) { return __x._M_w()._M_k == 1; }                                       \
    inline bool (isfinite)(T __x) { return __x._M_w()._M_k == 0; }                                    \
    inline bool (signbit)(T __x) { return __x._M_w()._M_c < 0; }                                      \
    inline bool (iszero)(T __x) { auto __w = __x._M_w(); return __w._M_k == 0 && __w._M_c == 0; }     \
    inline bool (isnormal)(T __x) { auto __w = __x._M_w(); if (__w._M_k || __w._M_c == 0) return false; \
        int __d = 0; for (__int128 __c = __w._M_c < 0 ? -__w._M_c : __w._M_c; __c; __c /= 10) ++__d;  \
        return __w._M_e + __d - 1 >= T::_W::_Emin; }                                                \
    inline bool (issubnormal)(T __x) { return isfinite(__x) && !iszero(__x) && !isnormal(__x); }     \
    inline int (fpclassify)(T __x) { return isnan(__x) ? FP_NAN : isinf(__x) ? FP_INFINITE : iszero(__x) ? FP_ZERO : isnormal(__x) ? FP_NORMAL : FP_SUBNORMAL; }
__PRISM_DFPC(__prism_dec32)
__PRISM_DFPC(__prism_dec64)
__PRISM_DFPC(__prism_dec128)
#undef __PRISM_DFPC
#define DEC_INFINITY (__prism_dec32(__prism_dec32::_W::_S_inf(false)))
#define DEC_NAN (__prism_dec32(__prism_dec32::_W::_S_nan()))
#define HUGE_VAL_D32 DEC_INFINITY
#define HUGE_VAL_D64 (__prism_dec64(__prism_dec64::_W::_S_inf(false)))
#define HUGE_VAL_D128 (__prism_dec128(__prism_dec128::_W::_S_inf(false)))

// ---- printf/scanf with %H %D %DD (variadic templates: arguments stay typed) ----
struct __prism_dfp_arg {
    enum _K { _I, _U, _F, _LF, _P, _D } _M_k;
    long long _M_i = 0; unsigned long long _M_u = 0; double _M_f = 0; long double _M_lf = 0; const void* _M_p = nullptr;
    __prism_decw<34, 6144> _M_d; int _M_dw = 0;
};
inline __prism_dfp_arg __prism_dfp_mk(int __v) { __prism_dfp_arg __a{__prism_dfp_arg::_I}; __a._M_i = __v; return __a; }
inline __prism_dfp_arg __prism_dfp_mk(long __v) { __prism_dfp_arg __a{__prism_dfp_arg::_I}; __a._M_i = __v; return __a; }
inline __prism_dfp_arg __prism_dfp_mk(long long __v) { __prism_dfp_arg __a{__prism_dfp_arg::_I}; __a._M_i = __v; return __a; }
inline __prism_dfp_arg __prism_dfp_mk(char __v) { return __prism_dfp_mk((int)__v); }
inline __prism_dfp_arg __prism_dfp_mk(signed char __v) { return __prism_dfp_mk((int)__v); }
inline __prism_dfp_arg __prism_dfp_mk(short __v) { return __prism_dfp_mk((int)__v); }
inline __prism_dfp_arg __prism_dfp_mk(bool __v) { return __prism_dfp_mk((int)__v); }
inline __prism_dfp_arg __prism_dfp_mk(unsigned char __v) { return __prism_dfp_mk((int)__v); }
inline __prism_dfp_arg __prism_dfp_mk(unsigned short __v) { return __prism_dfp_mk((int)__v); }
inline __prism_dfp_arg __prism_dfp_mk(unsigned __v) { __prism_dfp_arg __a{__prism_dfp_arg::_U}; __a._M_u = __v; return __a; }
inline __prism_dfp_arg __prism_dfp_mk(unsigned long __v) { __prism_dfp_arg __a{__prism_dfp_arg::_U}; __a._M_u = __v; return __a; }
inline __prism_dfp_arg __prism_dfp_mk(unsigned long long __v) { __prism_dfp_arg __a{__prism_dfp_arg::_U}; __a._M_u = __v; return __a; }
inline __prism_dfp_arg __prism_dfp_mk(float __v) { __prism_dfp_arg __a{__prism_dfp_arg::_F}; __a._M_f = __v; return __a; }
inline __prism_dfp_arg __prism_dfp_mk(double __v) { __prism_dfp_arg __a{__prism_dfp_arg::_F}; __a._M_f = __v; return __a; }
inline __prism_dfp_arg __prism_dfp_mk(long double __v) { __prism_dfp_arg __a{__prism_dfp_arg::_LF}; __a._M_lf = __v; return __a; }
inline __prism_dfp_arg __prism_dfp_mk(const void* __v) { __prism_dfp_arg __a{__prism_dfp_arg::_P}; __a._M_p = __v; return __a; }
inline __prism_dfp_arg __prism_dfp_mk(decltype(nullptr)) { return __prism_dfp_mk((const void*)0); }
template <int _P, int _E, typename _S, int _K, int _T, int _B>
inline __prism_dfp_arg __prism_dfp_mk(__prism_dbid<_P, _E, _S, _K, _T, _B> __v) {
    __prism_dfp_arg __a{__prism_dfp_arg::_D}; auto __w = __v._M_w();
    __a._M_d._M_c = __w._M_c; __a._M_d._M_e = __w._M_e; __a._M_d._M_k = __w._M_k; __a._M_dw = _K; return __a;
}

// Decimal text: digits from the coefficient. When a precision drops
// digits, the text is rounded half-even (FE_DEC_TONEAREST); C23 7.23.6.1
// says the current decimal mode applies, so under another mode printf
// rounding may differ in the last printed digit (stated limit).
inline int __prism_dfp_fmt_dec(char* __o, int __cap, const __prism_decw<34, 6144>& __w, char __c, int __prec, unsigned __fl) {
    prism_fpfmt::Out __out{__o, __cap};
    const bool __up = __c >= 'A' && __c <= 'Z';
    const char __lc = __up ? char(__c + 32) : __c;
    if (__w._M_c < 0) __out.put('-'); else if (__fl & prism_fpfmt::kPlus) __out.put('+'); else if (__fl & prism_fpfmt::kSpace) __out.put(' ');
    if (__w._M_k) { __out.puts(__w._M_k == 1 ? (__up ? "INF" : "inf") : (__up ? "NAN" : "nan")); return __out.len; }
    prism_fpfmt::Dec __v;
    unsigned __int128 __u = __w._M_c < 0 ? (unsigned __int128)(-__w._M_c) : (unsigned __int128)__w._M_c;
    char __t[40]; int __k = 0;
    do { __t[__k++] = char('0' + (int)(__u % 10)); __u /= 10; } while (__u);
    __v.n = __k; for (int __i = 0; __i < __k; ++__i) __v.d[__i] = __t[__k - 1 - __i];
    __v.x = __w._M_e;
    const bool __zero = __v.n == 1 && __v.d[0] == '0';
    const bool __alt = __fl & prism_fpfmt::kAlt;
    if (__lc == 'e' || __lc == 'a') {
        int __p = __prec >= 0 ? __prec : __v.n - 1;
        prism_fpfmt::Dec __r = __v;
        if (!__zero) prism_fpfmt::round_to(__r, __p + 1);
        // keep quantum digits: round_to strips trailing zeros; put_sci pads them back
        if (__zero) { __r.x = 0; }
        prism_fpfmt::put_sci(__out, __r, __p, __alt, __up);
        if (__zero) { /* exponent of a zero keeps its quantum */ }
        return __out.len;
    }
    if (__lc == 'f') {
        int __p = __prec >= 0 ? __prec : (__v.x < 0 ? -__v.x : 0);
        prism_fpfmt::Dec __r = __v;
        if (!__zero) prism_fpfmt::round_to(__r, __r.n + __r.x + __p); else __r.x = 0;
        prism_fpfmt::put_fixed(__out, __r, __p, __alt);
        return __out.len;
    }
    // g
    int __P = __prec < 0 ? 6 : (__prec == 0 ? 1 : __prec);
    prism_fpfmt::Dec __r = __v;
    if (!__zero) prism_fpfmt::round_to(__r, __P); else __r.x = 0;
    int __X = __zero ? 0 : __r.n - 1 + __r.x;
    char __b[400]; prism_fpfmt::Out __q{__b, 400};
    if (__P > __X && __X >= -4) { prism_fpfmt::Dec __f = __v; if (!__zero) prism_fpfmt::round_to(__f, __f.n + __f.x + (__P - 1 - __X)); else __f.x = 0; prism_fpfmt::put_fixed(__q, __f, __P - 1 - __X, __alt); }
    else prism_fpfmt::put_sci(__q, __r, __P - 1, __alt, __up);
    int __n = __q.len < 400 ? __q.len : 400, __ep = __n, __dot = -1;
    for (int __i = 0; __i < __n; ++__i) if (__b[__i] == 'e' || __b[__i] == 'E') { __ep = __i; break; }
    for (int __i = 0; __i < __ep; ++__i) if (__b[__i] == '.') __dot = __i;
    int __end = __ep;
    if (!__alt && __dot >= 0) { while (__end > __dot + 1 && __b[__end - 1] == '0') --__end; if (__end == __dot + 1) __end = __dot; }
    for (int __i = 0; __i < __end; ++__i) __out.put(__b[__i]);
    for (int __i = __ep; __i < __n; ++__i) __out.put(__b[__i]);
    return __out.len;
}

inline int __prism_dfp_vformat(char* __o, size_t __cap, const char* __f, const __prism_dfp_arg* __a, int __na) {
    size_t __len = 0; int __ai = 0;
    auto __put = [&](const char* __s, size_t __n) { for (size_t __i = 0; __i < __n; ++__i, ++__len) if (__len + 1 < __cap) __o[__len] = __s[__i]; };
    char __tmp[1200];
    while (*__f) {
        if (*__f != '%') { __put(__f, 1); ++__f; continue; }
        const char* __st = __f++;
        if (*__f == '%') { __put("%", 1); ++__f; continue; }
        unsigned __fl = 0; bool __left = false, __zpad = false;
        while (*__f && (*__f == '-' || *__f == '+' || *__f == ' ' || *__f == '#' || *__f == '0' || *__f == '\'')) {
            if (*__f == '-') __left = true; if (*__f == '+') __fl |= prism_fpfmt::kPlus; if (*__f == ' ') __fl |= prism_fpfmt::kSpace;
            if (*__f == '#') __fl |= prism_fpfmt::kAlt; if (*__f == '0') __zpad = true; ++__f; }
        int __w = 0, __p = -1;
        if (*__f == '*') { __w = __ai < __na ? (int)__a[__ai++]._M_i : 0; if (__w < 0) { __left = true; __w = -__w; } ++__f; }
        else while (*__f >= '0' && *__f <= '9') __w = __w * 10 + (*__f++ - '0');
        if (*__f == '.') { ++__f; __p = 0; if (*__f == '*') { __p = __ai < __na ? (int)__a[__ai++]._M_i : 0; ++__f; } else while (*__f >= '0' && *__f <= '9') __p = __p * 10 + (*__f++ - '0'); }
        const char* __lm = __f;
        while (*__f && (*__f == 'h' || *__f == 'l' || *__f == 'L' || *__f == 'j' || *__f == 'z' || *__f == 't' || *__f == 'q' || *__f == 'H' || *__f == 'D' || *__f == 'w' || (*__f >= '0' && *__f <= '9' && __f > __lm))) ++__f;
        const bool __dec = (__f - __lm == 1 && (*__lm == 'H' || *__lm == 'D')) || (__f - __lm == 2 && __lm[0] == 'D' && __lm[1] == 'D');
        const char __c = *__f ? *__f++ : 0;
        int __n;
        if (__dec) {
            __prism_check(__ai < __na && __a[__ai]._M_k == __prism_dfp_arg::_D ? 1 : 0, "FMT-ARGS", "%H/%D/%DD expects a decimal floating argument");
            __n = __prism_dfp_fmt_dec(__tmp, (int)sizeof __tmp, __a[__ai < __na ? __ai : 0]._M_d, __c, __p, __fl); ++__ai;
            if (__n > (int)sizeof __tmp) __n = (int)sizeof __tmp;
            int __sg = (__n > 0 && (__tmp[0] == '-' || __tmp[0] == '+' || __tmp[0] == ' ')) ? 1 : 0;
            if (__w > __n && !__left && __zpad) { __put(__tmp, (size_t)__sg); for (int __i = __n; __i < __w; ++__i) __put("0", 1); __put(__tmp + __sg, (size_t)(__n - __sg)); }
            else { if (__w > __n && !__left) for (int __i = __n; __i < __w; ++__i) __put(" ", 1); __put(__tmp, (size_t)__n); if (__w > __n && __left) for (int __i = __n; __i < __w; ++__i) __put(" ", 1); }
            continue;
        }
        // one C conversion through the C library, its own spec text
        char __sp[64]; size_t __sl = (size_t)(__f - __st); if (__sl > 63) __sl = 63;
        for (size_t __i = 0; __i < __sl; ++__i) __sp[__i] = __st[__i]; __sp[__sl] = 0;
        // '*' already consumed: rebuild with the numbers
        if (__builtin_strchr(__sp, '*')) { int __k = 0; char __s2[64]; __s2[__k++] = '%'; for (const char* __q = __sp + 1; *__q && __k < 40; ++__q) { if (*__q == '*') { __k += snprintf(__s2 + __k, 20, "%d", __q[-1] == '.' ? __p : (__left ? -__w : __w)); } else __s2[__k++] = *__q; } __s2[__k] = 0; for (int __i = 0; __i <= __k; ++__i) __sp[__i] = __s2[__i]; }
        const __prism_dfp_arg __x = __ai < __na ? __a[__ai] : __prism_dfp_mk(0); ++__ai;
        switch (__x._M_k) {
            case __prism_dfp_arg::_I: __n = (__c == 'c') ? snprintf(__tmp, sizeof __tmp, __sp, (int)__x._M_i) : (__lm != __f - 1 && *__lm == 'l') ? snprintf(__tmp, sizeof __tmp, __sp, (long long)__x._M_i) : snprintf(__tmp, sizeof __tmp, __sp, (int)__x._M_i); break;
            case __prism_dfp_arg::_U: __n = (__lm != __f - 1 && *__lm == 'l') ? snprintf(__tmp, sizeof __tmp, __sp, (unsigned long long)__x._M_u) : snprintf(__tmp, sizeof __tmp, __sp, (unsigned)__x._M_u); break;
            case __prism_dfp_arg::_F: __n = snprintf(__tmp, sizeof __tmp, __sp, __x._M_f); break;
            case __prism_dfp_arg::_LF: __n = snprintf(__tmp, sizeof __tmp, __sp, __x._M_lf); break;
            case __prism_dfp_arg::_P: __n = snprintf(__tmp, sizeof __tmp, __sp, __x._M_p); break;
            default: __prism_check(0, "FMT-ARGS", "decimal argument for a non-decimal conversion"); __n = 0; break;
        }
        if (__n > (int)sizeof __tmp - 1) __n = (int)sizeof __tmp - 1;
        if (__n > 0) __put(__tmp, (size_t)__n);
    }
    if (__cap) __o[__len < __cap ? __len : __cap - 1] = 0;
    return (int)__len;
}
template <typename... _A> inline int __prism_dfp_snprintf(char* __o, size_t __n, const char* __f, _A... __a) {
    const __prism_dfp_arg __v[sizeof...(_A) + 1] = {__prism_dfp_mk(__a)..., __prism_dfp_mk(0)};
    return __prism_dfp_vformat(__o, __n, __f, __v, (int)sizeof...(_A));
}
template <typename... _A> inline int __prism_dfp_sprintf(char* __o, const char* __f, _A... __a) { return __prism_dfp_snprintf(__o, (size_t)-1 / 2, __f, __a...); }
template <typename... _A> inline int __prism_dfp_fprintf(FILE* __s, const char* __f, _A... __a) {
    int __n = __prism_dfp_snprintf((char*)nullptr, 0, __f, __a...);
    char* __b = (char*)__builtin_alloca((size_t)__n + 1);
    __prism_dfp_snprintf(__b, (size_t)__n + 1, __f, __a...);
    return fputs(__b, __s) < 0 ? -1 : __n;
}
template <typename... _A> inline int __prism_dfp_printf(const char* __f, _A... __a) { return __prism_dfp_fprintf(stdout, __f, __a...); }

// scanf: decimal conversions parsed here, the rest by the C library per spec.
struct __prism_dfp_src { const char* _M_s; FILE* _M_f; int get() { if (_M_s) return *_M_s ? (unsigned char)*_M_s++ : -1; return getc(_M_f); } void unget(int __c) { if (__c < 0) return; if (_M_s) --_M_s; else ungetc(__c, _M_f); } };
template <typename _T> struct __prism_remove_ptr;
template <typename _T> struct __prism_remove_ptr<_T*> { using type = _T; };
struct __prism_dfp_ptr { void* _M_p; int _M_dw; };
inline __prism_dfp_ptr __prism_dfp_pk(void* __p) { return {__p, 0}; }
inline __prism_dfp_ptr __prism_dfp_pk(__prism_dec32* __p) { return {__p, 32}; }
inline __prism_dfp_ptr __prism_dfp_pk(__prism_dec64* __p) { return {__p, 64}; }
inline __prism_dfp_ptr __prism_dfp_pk(__prism_dec128* __p) { return {__p, 128}; }
inline int __prism_dfp_vscan(__prism_dfp_src __in, const char* __f, const __prism_dfp_ptr* __a, int __na) {
    int __got = 0, __ai = 0; bool __any = false;
    while (*__f) {
        if (*__f == ' ' || *__f == '\t' || *__f == '\n') { int __c; while ((__c = __in.get()) == ' ' || __c == '\t' || __c == '\n') {} __in.unget(__c); ++__f; continue; }
        if (*__f != '%' || __f[1] == '%') { int __c = __in.get(); if (__c != (unsigned char)*__f) { __in.unget(__c); break; } __f += *__f == '%' ? 2 : 1; continue; }
        const char* __st = __f++;
        bool __sup = *__f == '*'; if (__sup) ++__f;
        int __w = 0; while (*__f >= '0' && *__f <= '9') __w = __w * 10 + (*__f++ - '0');
        const char* __lm = __f;
        while (*__f && (*__f == 'h' || *__f == 'l' || *__f == 'L' || *__f == 'j' || *__f == 'z' || *__f == 't' || *__f == 'H' || *__f == 'D')) ++__f;
        const bool __dec = (__f - __lm == 1 && (*__lm == 'H' || *__lm == 'D')) || (__f - __lm == 2 && __lm[0] == 'D' && __lm[1] == 'D');
        const char __cv = *__f ? *__f++ : 0;
        if (__dec) {
            char __t[128]; int __k = 0, __c;
            while ((__c = __in.get()) == ' ' || __c == '\t' || __c == '\n') {}
            const int __lim = __w > 0 && __w < 127 ? __w : 127;
            auto __acc = [&](int __ch) { if (__k < __lim) { __t[__k++] = (char)__ch; return true; } return false; };
            if ((__c == '+' || __c == '-') && __acc(__c)) __c = __in.get();
            bool __dig = false;
            while (__c >= '0' && __c <= '9' && __acc(__c)) { __dig = true; __c = __in.get(); }
            if (__c == '.' && __acc(__c)) { __c = __in.get(); while (__c >= '0' && __c <= '9' && __acc(__c)) { __dig = true; __c = __in.get(); } }
            if (__dig && (__c == 'e' || __c == 'E') && __acc(__c)) { __c = __in.get(); if ((__c == '+' || __c == '-') && __acc(__c)) __c = __in.get(); while (__c >= '0' && __c <= '9' && __acc(__c)) __c = __in.get(); }
            bool __special = false;
            if (!__dig && (__c == 'i' || __c == 'I' || __c == 'n' || __c == 'N')) { char __w0 = (char)(__c | 32); __acc(__c); for (int __i = 0; __i < 2; ++__i) { __c = __in.get(); __acc(__c); } __c = __in.get(); __special = (__w0 == 'i' || __w0 == 'n'); }
            __in.unget(__c);
            __t[__k] = 0;
            if (!__dig && !__special) break;
            __any = true;
            if (!__sup) {
                const bool __neg = __t[0] == '-'; const char* __b = __t + ((__t[0] == '-' || __t[0] == '+') ? 1 : 0);
                const bool __isnan = (__b[0] | 32) == 'n', __isinf = (__b[0] | 32) == 'i';
                const __prism_dfp_ptr __p = __ai < __na ? __a[__ai] : __prism_dfp_ptr{nullptr, 0}; ++__ai;
                __prism_check(__p._M_dw != 0 ? 1 : 0, "FMT-ARGS", "%H/%D/%DD expects a pointer to a decimal floating object");
                auto __set = [&](auto* __d) { using _T = typename __prism_remove_ptr<decltype(__d)>::type; _T __v = __isnan ? _T(_T::_W::_S_nan()) : __isinf ? _T(_T::_W::_S_inf(__neg)) : __prism_dfp_parse<_T>(__b); if (__neg && !__isinf) __v = -__v; *__d = __v; };
                if (__p._M_dw == 32) __set((__prism_dec32*)__p._M_p); else if (__p._M_dw == 64) __set((__prism_dec64*)__p._M_p); else if (__p._M_dw == 128) __set((__prism_dec128*)__p._M_p);
                ++__got;
            }
            (void)__cv;
            continue;
        }
        // one C conversion through the C library: "<spec>%n" counts the consumed text
        char __sp[64]; size_t __sl = (size_t)(__f - __st); if (__sl > 60) __sl = 60;
        for (size_t __i = 0; __i < __sl; ++__i) __sp[__i] = __st[__i]; __sp[__sl] = '%'; __sp[__sl + 1] = 'n'; __sp[__sl + 2] = 0;
        int __used = -1, __r;
        void* __p = __sup ? nullptr : (__ai < __na ? __a[__ai++]._M_p : nullptr);
        if (__in._M_s) __r = __sup ? sscanf(__in._M_s, __sp, &__used) : sscanf(__in._M_s, __sp, __p, &__used);
        else __r = __sup ? fscanf(__in._M_f, __sp, &__used) : fscanf(__in._M_f, __sp, __p, &__used);
        if (__r == EOF && !__any) return EOF;
        if (__used < 0) break;
        __any = true;
        if (__in._M_s) __in._M_s += __used;
        if (!__sup && __cv != 'n') ++__got;
    }
    return __got;
}
template <typename... _A> inline int __prism_dfp_sscanf(const char* __s, const char* __f, _A... __a) {
    const __prism_dfp_ptr __v[sizeof...(_A) + 1] = {__prism_dfp_pk(__a)..., {nullptr, 0}};
    return __prism_dfp_vscan(__prism_dfp_src{__s, nullptr}, __f, __v, (int)sizeof...(_A));
}
template <typename... _A> inline int __prism_dfp_fscanf(FILE* __s, const char* __f, _A... __a) {
    const __prism_dfp_ptr __v[sizeof...(_A) + 1] = {__prism_dfp_pk(__a)..., {nullptr, 0}};
    return __prism_dfp_vscan(__prism_dfp_src{nullptr, __s}, __f, __v, (int)sizeof...(_A));
}
template <typename... _A> inline int __prism_dfp_scanf(const char* __f, _A... __a) { return __prism_dfp_fscanf(stdin, __f, __a...); }

// C spellings C++ lacks (only for C units lowered through this shim).
#ifdef __PRISM_DFP_C_UNIT
#define restrict __restrict
#define _Bool bool
#define _Noreturn [[noreturn]]
#define _Alignas alignas
#define _Alignof alignof
#define _Thread_local thread_local
#endif
#define _Decimal32 __prism_dec32
#define _Decimal64 __prism_dec64
#define _Decimal128 __prism_dec128
#define __STDC_IEC_60559_DFP__ 202311L
#define DEC32_MAX_EXP 97
#define DEC64_MAX_EXP 385
#define DEC128_MAX_EXP 6145
#define DEC32_MANT_DIG 7
#define DEC64_MANT_DIG 16
#define DEC128_MANT_DIG 34
#endif  // __cplusplus
#endif
