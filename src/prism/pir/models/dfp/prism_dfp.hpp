// PRISM decimal floating point shim (C23 _Decimal32/_Decimal64/_Decimal128).
//
// Why this exists: clang (checked: 18.1.8 and 23.1.4) rejects every
// _Decimal type with "GNU decimal type extension not supported"; there is
// no LLVM IR decimal type. GCC lowers these types to libgcc __bid_* calls,
// but PRISM analyses LLVM IR, so GCC output is unusable. The route PRISM
// controls: when a C unit fails with that diagnostic, the pir stage
// (stage.cpp, dfp_rewrite) rewrites decimal literal suffixes (1.5DD ->
// 1.5_prism_dd) and compiles the unit as C++23 with this header
// force-included. The keyword spellings are macros for the classes below,
// which implement IEEE 754-2008 decimal arithmetic in coefficient/exponent
// form (not BID bits): +, -, *, / with round-half-even (the C23 default
// decimal rounding mode, FE_DEC_TONEAREST), comparisons, conversions
// to/from integers and binary floating point, overflow to infinity,
// x/0 -> infinity, 0/0 -> NaN. Not modelled: other rounding modes
// (fe_dec_setround), quantum/quantize functions of <math.h>, the BID/DPD bit
// encoding (memcpy of a _Decimal object sees this layout, not IEEE bits),
// printf %H/%D/%DD, and C programs that are not valid C++ (implicit void*
// conversion etc.): those still fail to compile and are reported as such.
// This is not PROVED and not CLEAN.
#ifndef PRISM_DFP_HPP
#define PRISM_DFP_HPP 1
#ifdef __cplusplus

extern "C" void __prism_check(int ok, const char* cls, const char* msg);
inline void __prism_dfp_cast_check(int __ok) {
    __prism_check(__ok, "FP-CAST", "decimal floating value out of range of the integer type");
}

template <int _P, int _Emax>
struct __prism_dec {
    // value = _M_c * 10^_M_e, |_M_c| < 10^_P. _M_k: 0 finite, 1 inf, 2 nan.
    __int128 _M_c = 0;
    int _M_e = 0;
    int _M_k = 0;
    static constexpr int _Emin = 1 - _Emax;  // smallest normal exponent
    static constexpr __int128 _S_pow(int __n) { __int128 __r = 1; while (__n-- > 0) __r *= 10; return __r; }
    static constexpr __int128 _S_lim = _S_pow(_P);

    constexpr __prism_dec() = default;
    constexpr __prism_dec(int __v) { *this = _S_make(__v, 0); }
    constexpr __prism_dec(long __v) { *this = _S_make(__v, 0); }
    constexpr __prism_dec(long long __v) { *this = _S_make(__v, 0); }
    constexpr __prism_dec(unsigned __v) { *this = _S_make(__v, 0); }
    constexpr __prism_dec(unsigned long __v) { *this = _S_make(__v, 0); }
    constexpr __prism_dec(unsigned long long __v) { *this = _S_make(__v, 0); }
    // Binary -> decimal: exact for integers; otherwise rounded to _P digits
    // via a scaled integer (correct to within the double's own precision).
    __prism_dec(double __d) {
        if (__d != __d) { _M_k = 2; return; }
        if (__d > 1e300 || __d < -1e300) { _M_k = 1; _M_c = __d < 0 ? -1 : 1; return; }
        int __e = 0;
        double __a = __d < 0 ? -__d : __d;
        while (__a != 0 && __a < 1e15) { __a *= 10; --__e; }
        while (__a >= 1e16) { __a /= 10; ++__e; }
        *this = _S_make(static_cast<__int128>(__d < 0 ? -__a : __a), __e);
    }
    template <int _P2, int _E2>
    constexpr __prism_dec(const __prism_dec<_P2, _E2>& __o) { _M_k = __o._M_k; if (!_M_k) *this = _S_make(__o._M_c, __o._M_e); else _M_c = __o._M_c; }

    // Round __c*10^__e to _P digits, half-even; overflow -> inf.
    static constexpr __prism_dec _S_make(__int128 __c, int __e) {
        __prism_dec __r;
        bool __neg = __c < 0;
        unsigned __int128 __u = __neg ? (unsigned __int128)(-(__c + 1)) + 1 : (unsigned __int128)__c;
        while (__u >= (unsigned __int128)_S_lim || __e < _Emin - (_P - 1)) {
            unsigned __int128 __q = __u / 10, __rem = __u % 10;
            // sticky bit is folded in by the caller's extra digits; tie -> even
            if (__rem > 5 || (__rem == 5 && (__q & 1))) ++__q;
            __u = __q;
            ++__e;
            if (__u == 0) break;
        }
        if (__u != 0 && __e + 0 > _Emax - (_P - 1) + 0) {
            // normalise down if possible (clamp), else overflow
            while (__e > _Emax - (_P - 1) && __u * 10 < (unsigned __int128)_S_lim) { __u *= 10; --__e; }
            if (__e > _Emax - (_P - 1)) { __r._M_k = 1; __r._M_c = __neg ? -1 : 1; return __r; }
        }
        __r._M_c = __neg ? -(__int128)__u : (__int128)__u;
        __r._M_e = __u == 0 ? 0 : __e;
        return __r;
    }
    static constexpr __prism_dec _S_nan() { __prism_dec __r; __r._M_k = 2; return __r; }
    static constexpr __prism_dec _S_inf(bool __neg) { __prism_dec __r; __r._M_k = 1; __r._M_c = __neg ? -1 : 1; return __r; }

    // Align exponents keeping 3 guard digits (+ sticky) so _S_make rounds right.
    friend constexpr __prism_dec operator+(__prism_dec __a, __prism_dec __b) {
        if (__a._M_k == 2 || __b._M_k == 2) return _S_nan();
        if (__a._M_k == 1 || __b._M_k == 1) {
            if (__a._M_k == 1 && __b._M_k == 1 && (__a._M_c < 0) != (__b._M_c < 0)) return _S_nan();
            return __a._M_k == 1 ? __a : __b;
        }
        if (__a._M_c == 0) return __b;
        if (__b._M_c == 0) return __a;
        if (__a._M_e < __b._M_e) { __prism_dec __t = __a; __a = __b; __b = __t; }
        // raise __a's coefficient while it fits
        while (__a._M_e > __b._M_e && (__a._M_c < 0 ? -__a._M_c : __a._M_c) < _S_pow(36)) { __a._M_c *= 10; --__a._M_e; }
        bool __sticky = false;
        while (__b._M_e < __a._M_e) { if (__b._M_c % 10) __sticky = true; __b._M_c /= 10; ++__b._M_e; }
        __int128 __s = __a._M_c + __b._M_c;
        if (__sticky) { __s = __s * 10 + (__b._M_c < 0 ? -1 : 1); return _S_make(__s, __a._M_e - 1); }
        return _S_make(__s, __a._M_e);
    }
    friend constexpr __prism_dec operator-(__prism_dec __a) { if (__a._M_k != 2) __a._M_c = -__a._M_c; return __a; }
    friend constexpr __prism_dec operator+(__prism_dec __a) { return __a; }
    friend constexpr __prism_dec operator-(__prism_dec __a, __prism_dec __b) { return __a + (-__b); }
    friend constexpr __prism_dec operator*(__prism_dec __a, __prism_dec __b) {
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
    friend constexpr __prism_dec operator/(__prism_dec __a, __prism_dec __b) {
        if (__a._M_k == 2 || __b._M_k == 2) return _S_nan();
        bool __neg = (__a._M_c < 0) != (__b._M_c < 0);
        if (__a._M_k == 1) return __b._M_k == 1 ? _S_nan() : _S_inf(__neg);
        if (__b._M_k == 1) return __prism_dec{};
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
    __prism_dec& operator+=(__prism_dec __o) { return *this = *this + __o; }
    __prism_dec& operator-=(__prism_dec __o) { return *this = *this - __o; }
    __prism_dec& operator*=(__prism_dec __o) { return *this = *this * __o; }
    __prism_dec& operator/=(__prism_dec __o) { return *this = *this / __o; }
    __prism_dec& operator++() { return *this += __prism_dec(1); }
    __prism_dec& operator--() { return *this -= __prism_dec(1); }
    __prism_dec operator++(int) { __prism_dec __t = *this; ++*this; return __t; }
    __prism_dec operator--(int) { __prism_dec __t = *this; --*this; return __t; }

    // -1, 0, 1; 2 when unordered (NaN).
    static constexpr int _S_cmp(__prism_dec __a, __prism_dec __b) {
        if (__a._M_k == 2 || __b._M_k == 2) return 2;
        __prism_dec __d = __a - __b;
        if (__d._M_k == 2) return 2;
        return __d._M_c < 0 ? -1 : __d._M_c > 0 ? 1 : 0;
    }
    friend constexpr bool operator==(__prism_dec __a, __prism_dec __b) { return _S_cmp(__a, __b) == 0; }
    friend constexpr bool operator!=(__prism_dec __a, __prism_dec __b) { return _S_cmp(__a, __b) != 0; }
    friend constexpr bool operator<(__prism_dec __a, __prism_dec __b) { return _S_cmp(__a, __b) == -1; }
    friend constexpr bool operator>(__prism_dec __a, __prism_dec __b) { return _S_cmp(__a, __b) == 1; }
    friend constexpr bool operator<=(__prism_dec __a, __prism_dec __b) { int __c = _S_cmp(__a, __b); return __c == -1 || __c == 0; }
    friend constexpr bool operator>=(__prism_dec __a, __prism_dec __b) { int __c = _S_cmp(__a, __b); return __c == 1 || __c == 0; }
    constexpr explicit operator bool() const { return _M_k != 0 || _M_c != 0; }
    constexpr bool operator!() const { return !static_cast<bool>(*this); }

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


using __prism_dec32 = __prism_dec<7, 96>;
using __prism_dec64 = __prism_dec<16, 384>;
using __prism_dec128 = __prism_dec<34, 6144>;

// Exact decimal literal parse: digits[.digits][e[+-]digits].
template <typename _D>
constexpr _D __prism_dfp_parse(const char* __s) {
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
    return _D::_S_make(__c, __e + __xs * __x);
}
constexpr __prism_dec32 operator""_prism_df(const char* __s) { return __prism_dfp_parse<__prism_dec32>(__s); }
constexpr __prism_dec64 operator""_prism_dd(const char* __s) { return __prism_dfp_parse<__prism_dec64>(__s); }
constexpr __prism_dec128 operator""_prism_dl(const char* __s) { return __prism_dfp_parse<__prism_dec128>(__s); }

// Mixed-width arithmetic promotes to the wider type (C23 6.3.1.8).
#define __PRISM_DFP_MIX(A, B)                                                  \
    inline B operator+(A __a, B __b) { return B(__a) + __b; }                   \
    inline B operator+(B __a, A __b) { return __a + B(__b); }                   \
    inline B operator-(A __a, B __b) { return B(__a) - __b; }                   \
    inline B operator-(B __a, A __b) { return __a - B(__b); }                   \
    inline B operator*(A __a, B __b) { return B(__a) * __b; }                   \
    inline B operator*(B __a, A __b) { return __a * B(__b); }                   \
    inline B operator/(A __a, B __b) { return B(__a) / __b; }                   \
    inline B operator/(B __a, A __b) { return __a / B(__b); }
__PRISM_DFP_MIX(__prism_dec32, __prism_dec64)
__PRISM_DFP_MIX(__prism_dec32, __prism_dec128)
__PRISM_DFP_MIX(__prism_dec64, __prism_dec128)
#undef __PRISM_DFP_MIX

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
