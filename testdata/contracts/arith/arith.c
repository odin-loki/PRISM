/* Integer helpers the kernel calls everywhere (include/linux/log2.h,
 * math.h, math64.h, align.h shapes): ilog2, roundup_pow_of_two,
 * is_power_of_2, DIV_ROUND_UP, ALIGN, div_u64_rem, clamp. Scalar: the
 * contracts stage proves them directly. */
/* --- contract-library prelude (testdata/contracts/README.md) --- */
#include <assert.h>
/* --- end prelude --- */

/* The kernel's __ilog2_u32 / __ilog2_u64 (fls(n) - 1): -1 for 0. */
/*@ assigns \nothing;
    ensures n == 0 ==> \result == -1;
    ensures n != 0 ==> 0 <= \result <= 31 && (1u << \result) <= n;
    ensures n != 0 ==> \result == 31 || n < (1u << (\result + 1));
*/
int __ilog2_u32(unsigned n) { return n ? 31 - __builtin_clz(n) : -1; }

/*@ assigns \nothing;
    ensures n == 0 ==> \result == -1;
    ensures n != 0 ==> 0 <= \result <= 63 && (1ull << \result) <= n;
    ensures n != 0 ==> \result == 63 || n < (1ull << (\result + 1));
*/
int __ilog2_u64(unsigned long long n) { return n ? 63 - __builtin_clzll(n) : -1; }

/*@ assigns \nothing;
    ensures \result == (n != 0 && (n & (n - 1)) == 0);
*/
int is_power_of_2(unsigned long long n) { return n != 0 && (n & (n - 1)) == 0; }

/*@ requires 1 <= n <= 0x80000000u;
    assigns \nothing;
    ensures is_power_of_2(\result);
    ensures n <= \result && \result < 2ull * n;
*/
unsigned long long roundup_pow_of_two(unsigned n) {
    if (n == 1) return 1;
    return 1ull << (32 - __builtin_clz(n - 1));
}

/*@ requires d != 0;
    requires n <= 0xffffffffu - (d - 1);
    assigns \nothing;
    ensures \result * d >= n && (\result == 0 || (\result - 1) * d < n);
*/
unsigned div_round_up(unsigned n, unsigned d) { return (n + d - 1) / d; }

/* ALIGN(x, a) for a power-of-two a that does not overflow */
/*@ requires a != 0 && (a & (a - 1)) == 0;
    requires x <= 0xffffffffu - (a - 1);
    assigns \nothing;
    ensures \result >= x && \result - x < a && (\result & (a - 1)) == 0;
*/
unsigned align_up(unsigned x, unsigned a) { return (x + a - 1) & ~(a - 1); }

/* 64 by 32 division with remainder (div_u64_rem) */
/*@ requires \valid(rem);
    requires d != 0;
    assigns *rem;
    ensures *rem < d && \result * d + *rem == n;
*/
unsigned long long div_u64_rem(unsigned long long n, unsigned d, unsigned *rem) {
    *rem = (unsigned)(n % d);
    return n / d;
}

/*@ requires lo <= hi;
    assigns \nothing;
    ensures lo <= \result <= hi;
    ensures lo <= v <= hi ==> \result == v;
*/
int clamp_int(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

/* Planted twin of a contract: roundup of 0 (requires dropped). */
/*@ requires n <= 0x80000000u;
    ensures n <= \result;
*/
unsigned long long roundup_zero_bad(unsigned n) { return n == 0 ? 0 : roundup_pow_of_two(n) - 1; }

/* Planted twin: ALIGN without the overflow requires wraps to 0. */
/*@ requires a != 0 && (a & (a - 1)) == 0;
    ensures \result >= x;
*/
unsigned align_up_wrap_bad(unsigned x, unsigned a) { return (x + a - 1) & ~(a - 1); }
