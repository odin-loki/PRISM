/* PRISM hard benchmark: 64-bit multiply/shift arithmetic, nested and
 * unbounded loops (no pointer parameters). See README.md. */
#include <stdint.h>

/* 64-bit FNV-1a over a 64-byte derived block (constant trip count). */
uint64_t fnv64_ok(uint64_t x) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (int i = 0; i < 64; i++) {
        h ^= (x >> (i & 63)) & 0xff;
        h *= 0x100000001b3ull;
    }
    return h;
}

/* xorshift-multiply mixer, 128 rounds. */
uint64_t mix128_ok(uint64_t s) {
    for (int i = 0; i < 128; i++) {
        s ^= s >> 31;
        s *= 0x7fb5d329728ea185ull;
        s ^= s << 17;
    }
    return s;
}

/* Signed 64-bit polynomial evaluation: x*x*x overflows. */
int64_t poly_bad(int64_t x) {
    if (x < -3000000 || x > 3000000) return 0;
    int64_t r = 0;
    for (int i = 0; i < 4; i++) r = r * x + (int64_t)(i + 1);   /* x^3 * ... overflows */
    return r;
}

/* Fixed-point multiply with the range check done correctly. */
int32_t fixmul_ok(int32_t a, int32_t b) {
    if (a < -65536 || a > 65536 || b < -65536 || b > 65536) return 0;
    int64_t p = (int64_t)a * b;
    return (int32_t)(p >> 16);
}

/* Nested loops: an 16x16 accumulation into a 64-bit total (no overflow). */
int64_t nested_ok(int32_t a) {
    int64_t t = 0;
    for (int i = 0; i < 16; i++)
        for (int j = 0; j < 16; j++)
            t += (int64_t)a * (i ^ j);
    return t;
}

/* Nested loops with a 32-bit accumulator. Named _bad when written, but it
 * does not overflow: 144 * 1e7 = 1.44e9 < INT_MAX (2.147e9). PRISM proves it
 * (PROVED-UNBOUNDED, idiom round, 15 Nov 2026); the name is kept so earlier
 * measurements stay comparable. */
int nested_bad(int a) {
    if (a < 0 || a > 10000000) return 0;
    int t = 0;
    for (int i = 0; i < 12; i++)
        for (int j = 0; j < 12; j++)
            t += a;                       /* at most 144 * 1e7 = 1.44e9 */
    return t;
}

/* Unbounded countdown with a 64-bit accumulator kept small by a mask. */
uint64_t unbounded_mask_ok(uint64_t n, uint64_t k) {
    uint64_t acc = 0;
    while (n != 0) {
        acc = (acc * 31 + (k & 0xff)) & 0xffffffffull;
        n--;
    }
    return acc;
}

/* Unbounded loop: the index stays in 0..7 by the mask (needs an invariant). */
int unbounded_index_ok(int n, int v) {
    int a[8] = {0};
    int i = 0;
    while (n > 0) {
        a[i] += v & 1;
        i = (i + 1) & 7;
        n--;
    }
    return a[0];
}

/* Unbounded loop with a division whose divisor reaches zero. */
int unbounded_div_bad(int n, int x) {
    int d = 5, r = 0;
    while (n > 0) {
        r ^= x / d;                       /* d == 0 on the 6th iteration */
        d--;
        n--;
    }
    return r;
}

/* Bit-reversal of 64-bit words: shifts are all < 64. */
uint64_t bitrev_ok(uint64_t x) {
    uint64_t r = 0;
    for (int i = 0; i < 64; i++) {
        r = (r << 1) | (x & 1);
        x >>= 1;
    }
    return r;
}

/* Modular exponentiation by squaring over 64-bit values reduced mod a
 * 32-bit modulus (the products fit). */
uint64_t modexp_ok(uint64_t b, uint64_t e, uint32_t m) {
    if (m == 0) return 0;
    uint64_t r = 1 % m;
    b %= m;
    for (int i = 0; i < 64; i++) {
        if ((e >> i) & 1) r = (r * b) % m;
        b = (b * b) % m;
    }
    return r;
}
