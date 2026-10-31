/* <stdbit.h> (C23 7.18) for PIR: the frontend's libc may predate C23
 * (glibc < 2.39). Searched after the system headers (-idirafter), so a
 * libc that ships <stdbit.h> wins. Every function is total (zero input
 * handled explicitly; the builtins are never called with 0), so the
 * model adds no undefined behaviour C23 does not have. bit_ceil of a
 * value whose ceiling is unrepresentable returns 0 (C23: unspecified). */
#ifndef PRISM_PIR_STDBIT_H
#define PRISM_PIR_STDBIT_H
#define __STDC_VERSION_STDBIT_H__ 202311L
#define __STDC_ENDIAN_LITTLE__ 1234
#define __STDC_ENDIAN_BIG__ 4321
#define __STDC_ENDIAN_NATIVE__ __STDC_ENDIAN_LITTLE__
static inline unsigned int stdc_leading_zeros_uc(unsigned char x) { return x ? (unsigned int)(__builtin_clzll((unsigned long long)x) - (64 - 8)) : 8u; }
static inline unsigned int stdc_leading_ones_uc(unsigned char x) { return stdc_leading_zeros_uc((unsigned char)~x); }
static inline unsigned int stdc_trailing_zeros_uc(unsigned char x) { return x ? (unsigned int)(__builtin_ctzll((unsigned long long)x)) : 8u; }
static inline unsigned int stdc_trailing_ones_uc(unsigned char x) { return stdc_trailing_zeros_uc((unsigned char)~x); }
static inline unsigned int stdc_first_leading_zero_uc(unsigned char x) { return (unsigned char)~x ? stdc_leading_ones_uc(x) + 1u : 0u; }
static inline unsigned int stdc_first_leading_one_uc(unsigned char x) { return x ? stdc_leading_zeros_uc(x) + 1u : 0u; }
static inline unsigned int stdc_first_trailing_zero_uc(unsigned char x) { return (unsigned char)~x ? stdc_trailing_ones_uc(x) + 1u : 0u; }
static inline unsigned int stdc_first_trailing_one_uc(unsigned char x) { return x ? stdc_trailing_zeros_uc(x) + 1u : 0u; }
static inline unsigned int stdc_count_ones_uc(unsigned char x) { return (unsigned int)(__builtin_popcountll((unsigned long long)x)); }
static inline unsigned int stdc_count_zeros_uc(unsigned char x) { return 8u - stdc_count_ones_uc(x); }
static inline _Bool stdc_has_single_bit_uc(unsigned char x) { return stdc_count_ones_uc(x) == 1u; }
static inline unsigned int stdc_bit_width_uc(unsigned char x) { return 8u - stdc_leading_zeros_uc(x); }
static inline unsigned char stdc_bit_floor_uc(unsigned char x) { return x ? (unsigned char)((unsigned char)1 << (stdc_bit_width_uc(x) - 1u)) : (unsigned char)0; }
static inline unsigned char stdc_bit_ceil_uc(unsigned char x) {
    if (x <= 1) return (unsigned char)1;
    unsigned int w = stdc_bit_width_uc((unsigned char)(x - 1));
    return w < 8u ? (unsigned char)((unsigned char)1 << w) : (unsigned char)0;
}
static inline unsigned int stdc_leading_zeros_us(unsigned short x) { return x ? (unsigned int)(__builtin_clzll((unsigned long long)x) - (64 - 16)) : 16u; }
static inline unsigned int stdc_leading_ones_us(unsigned short x) { return stdc_leading_zeros_us((unsigned short)~x); }
static inline unsigned int stdc_trailing_zeros_us(unsigned short x) { return x ? (unsigned int)(__builtin_ctzll((unsigned long long)x)) : 16u; }
static inline unsigned int stdc_trailing_ones_us(unsigned short x) { return stdc_trailing_zeros_us((unsigned short)~x); }
static inline unsigned int stdc_first_leading_zero_us(unsigned short x) { return (unsigned short)~x ? stdc_leading_ones_us(x) + 1u : 0u; }
static inline unsigned int stdc_first_leading_one_us(unsigned short x) { return x ? stdc_leading_zeros_us(x) + 1u : 0u; }
static inline unsigned int stdc_first_trailing_zero_us(unsigned short x) { return (unsigned short)~x ? stdc_trailing_ones_us(x) + 1u : 0u; }
static inline unsigned int stdc_first_trailing_one_us(unsigned short x) { return x ? stdc_trailing_zeros_us(x) + 1u : 0u; }
static inline unsigned int stdc_count_ones_us(unsigned short x) { return (unsigned int)(__builtin_popcountll((unsigned long long)x)); }
static inline unsigned int stdc_count_zeros_us(unsigned short x) { return 16u - stdc_count_ones_us(x); }
static inline _Bool stdc_has_single_bit_us(unsigned short x) { return stdc_count_ones_us(x) == 1u; }
static inline unsigned int stdc_bit_width_us(unsigned short x) { return 16u - stdc_leading_zeros_us(x); }
static inline unsigned short stdc_bit_floor_us(unsigned short x) { return x ? (unsigned short)((unsigned short)1 << (stdc_bit_width_us(x) - 1u)) : (unsigned short)0; }
static inline unsigned short stdc_bit_ceil_us(unsigned short x) {
    if (x <= 1) return (unsigned short)1;
    unsigned int w = stdc_bit_width_us((unsigned short)(x - 1));
    return w < 16u ? (unsigned short)((unsigned short)1 << w) : (unsigned short)0;
}
static inline unsigned int stdc_leading_zeros_ui(unsigned int x) { return x ? (unsigned int)(__builtin_clzll((unsigned long long)x) - (64 - 32)) : 32u; }
static inline unsigned int stdc_leading_ones_ui(unsigned int x) { return stdc_leading_zeros_ui((unsigned int)~x); }
static inline unsigned int stdc_trailing_zeros_ui(unsigned int x) { return x ? (unsigned int)(__builtin_ctzll((unsigned long long)x)) : 32u; }
static inline unsigned int stdc_trailing_ones_ui(unsigned int x) { return stdc_trailing_zeros_ui((unsigned int)~x); }
static inline unsigned int stdc_first_leading_zero_ui(unsigned int x) { return (unsigned int)~x ? stdc_leading_ones_ui(x) + 1u : 0u; }
static inline unsigned int stdc_first_leading_one_ui(unsigned int x) { return x ? stdc_leading_zeros_ui(x) + 1u : 0u; }
static inline unsigned int stdc_first_trailing_zero_ui(unsigned int x) { return (unsigned int)~x ? stdc_trailing_ones_ui(x) + 1u : 0u; }
static inline unsigned int stdc_first_trailing_one_ui(unsigned int x) { return x ? stdc_trailing_zeros_ui(x) + 1u : 0u; }
static inline unsigned int stdc_count_ones_ui(unsigned int x) { return (unsigned int)(__builtin_popcountll((unsigned long long)x)); }
static inline unsigned int stdc_count_zeros_ui(unsigned int x) { return 32u - stdc_count_ones_ui(x); }
static inline _Bool stdc_has_single_bit_ui(unsigned int x) { return stdc_count_ones_ui(x) == 1u; }
static inline unsigned int stdc_bit_width_ui(unsigned int x) { return 32u - stdc_leading_zeros_ui(x); }
static inline unsigned int stdc_bit_floor_ui(unsigned int x) { return x ? (unsigned int)((unsigned int)1 << (stdc_bit_width_ui(x) - 1u)) : (unsigned int)0; }
static inline unsigned int stdc_bit_ceil_ui(unsigned int x) {
    if (x <= 1) return (unsigned int)1;
    unsigned int w = stdc_bit_width_ui((unsigned int)(x - 1));
    return w < 32u ? (unsigned int)((unsigned int)1 << w) : (unsigned int)0;
}
static inline unsigned int stdc_leading_zeros_ul(unsigned long x) { return x ? (unsigned int)(__builtin_clzll((unsigned long long)x) - (64 - 64)) : 64u; }
static inline unsigned int stdc_leading_ones_ul(unsigned long x) { return stdc_leading_zeros_ul((unsigned long)~x); }
static inline unsigned int stdc_trailing_zeros_ul(unsigned long x) { return x ? (unsigned int)(__builtin_ctzll((unsigned long long)x)) : 64u; }
static inline unsigned int stdc_trailing_ones_ul(unsigned long x) { return stdc_trailing_zeros_ul((unsigned long)~x); }
static inline unsigned int stdc_first_leading_zero_ul(unsigned long x) { return (unsigned long)~x ? stdc_leading_ones_ul(x) + 1u : 0u; }
static inline unsigned int stdc_first_leading_one_ul(unsigned long x) { return x ? stdc_leading_zeros_ul(x) + 1u : 0u; }
static inline unsigned int stdc_first_trailing_zero_ul(unsigned long x) { return (unsigned long)~x ? stdc_trailing_ones_ul(x) + 1u : 0u; }
static inline unsigned int stdc_first_trailing_one_ul(unsigned long x) { return x ? stdc_trailing_zeros_ul(x) + 1u : 0u; }
static inline unsigned int stdc_count_ones_ul(unsigned long x) { return (unsigned int)(__builtin_popcountll((unsigned long long)x)); }
static inline unsigned int stdc_count_zeros_ul(unsigned long x) { return 64u - stdc_count_ones_ul(x); }
static inline _Bool stdc_has_single_bit_ul(unsigned long x) { return stdc_count_ones_ul(x) == 1u; }
static inline unsigned int stdc_bit_width_ul(unsigned long x) { return 64u - stdc_leading_zeros_ul(x); }
static inline unsigned long stdc_bit_floor_ul(unsigned long x) { return x ? (unsigned long)((unsigned long)1 << (stdc_bit_width_ul(x) - 1u)) : (unsigned long)0; }
static inline unsigned long stdc_bit_ceil_ul(unsigned long x) {
    if (x <= 1) return (unsigned long)1;
    unsigned int w = stdc_bit_width_ul((unsigned long)(x - 1));
    return w < 64u ? (unsigned long)((unsigned long)1 << w) : (unsigned long)0;
}
static inline unsigned int stdc_leading_zeros_ull(unsigned long long x) { return x ? (unsigned int)(__builtin_clzll((unsigned long long)x) - (64 - 64)) : 64u; }
static inline unsigned int stdc_leading_ones_ull(unsigned long long x) { return stdc_leading_zeros_ull((unsigned long long)~x); }
static inline unsigned int stdc_trailing_zeros_ull(unsigned long long x) { return x ? (unsigned int)(__builtin_ctzll((unsigned long long)x)) : 64u; }
static inline unsigned int stdc_trailing_ones_ull(unsigned long long x) { return stdc_trailing_zeros_ull((unsigned long long)~x); }
static inline unsigned int stdc_first_leading_zero_ull(unsigned long long x) { return (unsigned long long)~x ? stdc_leading_ones_ull(x) + 1u : 0u; }
static inline unsigned int stdc_first_leading_one_ull(unsigned long long x) { return x ? stdc_leading_zeros_ull(x) + 1u : 0u; }
static inline unsigned int stdc_first_trailing_zero_ull(unsigned long long x) { return (unsigned long long)~x ? stdc_trailing_ones_ull(x) + 1u : 0u; }
static inline unsigned int stdc_first_trailing_one_ull(unsigned long long x) { return x ? stdc_trailing_zeros_ull(x) + 1u : 0u; }
static inline unsigned int stdc_count_ones_ull(unsigned long long x) { return (unsigned int)(__builtin_popcountll((unsigned long long)x)); }
static inline unsigned int stdc_count_zeros_ull(unsigned long long x) { return 64u - stdc_count_ones_ull(x); }
static inline _Bool stdc_has_single_bit_ull(unsigned long long x) { return stdc_count_ones_ull(x) == 1u; }
static inline unsigned int stdc_bit_width_ull(unsigned long long x) { return 64u - stdc_leading_zeros_ull(x); }
static inline unsigned long long stdc_bit_floor_ull(unsigned long long x) { return x ? (unsigned long long)((unsigned long long)1 << (stdc_bit_width_ull(x) - 1u)) : (unsigned long long)0; }
static inline unsigned long long stdc_bit_ceil_ull(unsigned long long x) {
    if (x <= 1) return (unsigned long long)1;
    unsigned int w = stdc_bit_width_ull((unsigned long long)(x - 1));
    return w < 64u ? (unsigned long long)((unsigned long long)1 << w) : (unsigned long long)0;
}
#define stdc_leading_zeros(x) _Generic((x), unsigned char: stdc_leading_zeros_uc, unsigned short: stdc_leading_zeros_us, unsigned int: stdc_leading_zeros_ui, unsigned long: stdc_leading_zeros_ul, unsigned long long: stdc_leading_zeros_ull)(x)
#define stdc_leading_ones(x) _Generic((x), unsigned char: stdc_leading_ones_uc, unsigned short: stdc_leading_ones_us, unsigned int: stdc_leading_ones_ui, unsigned long: stdc_leading_ones_ul, unsigned long long: stdc_leading_ones_ull)(x)
#define stdc_trailing_zeros(x) _Generic((x), unsigned char: stdc_trailing_zeros_uc, unsigned short: stdc_trailing_zeros_us, unsigned int: stdc_trailing_zeros_ui, unsigned long: stdc_trailing_zeros_ul, unsigned long long: stdc_trailing_zeros_ull)(x)
#define stdc_trailing_ones(x) _Generic((x), unsigned char: stdc_trailing_ones_uc, unsigned short: stdc_trailing_ones_us, unsigned int: stdc_trailing_ones_ui, unsigned long: stdc_trailing_ones_ul, unsigned long long: stdc_trailing_ones_ull)(x)
#define stdc_first_leading_zero(x) _Generic((x), unsigned char: stdc_first_leading_zero_uc, unsigned short: stdc_first_leading_zero_us, unsigned int: stdc_first_leading_zero_ui, unsigned long: stdc_first_leading_zero_ul, unsigned long long: stdc_first_leading_zero_ull)(x)
#define stdc_first_leading_one(x) _Generic((x), unsigned char: stdc_first_leading_one_uc, unsigned short: stdc_first_leading_one_us, unsigned int: stdc_first_leading_one_ui, unsigned long: stdc_first_leading_one_ul, unsigned long long: stdc_first_leading_one_ull)(x)
#define stdc_first_trailing_zero(x) _Generic((x), unsigned char: stdc_first_trailing_zero_uc, unsigned short: stdc_first_trailing_zero_us, unsigned int: stdc_first_trailing_zero_ui, unsigned long: stdc_first_trailing_zero_ul, unsigned long long: stdc_first_trailing_zero_ull)(x)
#define stdc_first_trailing_one(x) _Generic((x), unsigned char: stdc_first_trailing_one_uc, unsigned short: stdc_first_trailing_one_us, unsigned int: stdc_first_trailing_one_ui, unsigned long: stdc_first_trailing_one_ul, unsigned long long: stdc_first_trailing_one_ull)(x)
#define stdc_count_ones(x) _Generic((x), unsigned char: stdc_count_ones_uc, unsigned short: stdc_count_ones_us, unsigned int: stdc_count_ones_ui, unsigned long: stdc_count_ones_ul, unsigned long long: stdc_count_ones_ull)(x)
#define stdc_count_zeros(x) _Generic((x), unsigned char: stdc_count_zeros_uc, unsigned short: stdc_count_zeros_us, unsigned int: stdc_count_zeros_ui, unsigned long: stdc_count_zeros_ul, unsigned long long: stdc_count_zeros_ull)(x)
#define stdc_has_single_bit(x) _Generic((x), unsigned char: stdc_has_single_bit_uc, unsigned short: stdc_has_single_bit_us, unsigned int: stdc_has_single_bit_ui, unsigned long: stdc_has_single_bit_ul, unsigned long long: stdc_has_single_bit_ull)(x)
#define stdc_bit_width(x) _Generic((x), unsigned char: stdc_bit_width_uc, unsigned short: stdc_bit_width_us, unsigned int: stdc_bit_width_ui, unsigned long: stdc_bit_width_ul, unsigned long long: stdc_bit_width_ull)(x)
#define stdc_bit_floor(x) _Generic((x), unsigned char: stdc_bit_floor_uc, unsigned short: stdc_bit_floor_us, unsigned int: stdc_bit_floor_ui, unsigned long: stdc_bit_floor_ul, unsigned long long: stdc_bit_floor_ull)(x)
#define stdc_bit_ceil(x) _Generic((x), unsigned char: stdc_bit_ceil_uc, unsigned short: stdc_bit_ceil_us, unsigned int: stdc_bit_ceil_ui, unsigned long: stdc_bit_ceil_ul, unsigned long long: stdc_bit_ceil_ull)(x)
#endif
