/* PIR tasks: exact x86-64 semantics of the inline asm templates the kernel uses
 * (docs/ASM_CENSUS.md, src/prism/pir/translate_asm.inc). Run with --kernel-api linux.
 * Each template has a true variant (name_ok: PROVED-ASSUMING asm-exact) and a false one
 * (name_bad: FAILED). A property is written as a divisor that is 0 when it is false:
 * 100 / (property) reports INT-DIV-ZERO exactly when the property can fail.
 * cpuid is privileged (asm-privileged: outputs havoc); rdtsc is asm-nondet. */
typedef unsigned long u64;
typedef unsigned int u32;

/* bswap */
int bswap_ok(u64 x) {
    u64 a = x, b;
    __asm__("bswapq %0" : "+r"(a));
    b = a;
    __asm__("bswapq %0" : "+r"(b));
    return 100 / (b == x);
}
int bswap_bad(u64 x) {
    u64 a = x;
    __asm__("bswapq %0" : "+r"(a));
    return 100 / (a == x);
}

/* popcnt */
int popcnt_ok(u64 x) {
    u64 r;
    __asm__("popcntq %1, %0" : "=r"(r) : "r"(x) : "cc");
    return 100 / (r <= 64);
}
int popcnt_bad(u64 x) {
    u64 r;
    __asm__("popcntq %1, %0" : "=r"(r) : "r"(x) : "cc");
    return 100 / (r < 64);
}

/* bsf: lowest set bit (destination undefined for 0, so only used on x != 0) */
int bsf_ok(u64 x) {
    u64 r;
    if (x == 0) return 0;
    __asm__("bsfq %1, %0" : "=r"(r) : "r"(x) : "cc");
    return 100 / (int)((x >> r) & 1);
}
int bsf_bad(u64 x) {
    u64 r;
    if (x == 0) return 0;
    __asm__("bsfq %1, %0" : "=r"(r) : "r"(x) : "cc");
    return 100 / (int)((x >> r >> 1) & 1); /* the bit above the lowest one may be clear */
}

/* rol / ror by %cl */
int rotate_ok(u64 x, u32 c) {
    u64 a = x;
    __asm__("rolq %%cl, %0" : "+r"(a) : "c"(c) : "cc");
    __asm__("rorq %%cl, %0" : "+r"(a) : "c"(c) : "cc");
    return 100 / (a == x);
}
int rotate_bad(u64 x, u32 c) {
    u64 a = x;
    __asm__("rolq %%cl, %0" : "+r"(a) : "c"(c) : "cc");
    return 100 / (a == x);
}

/* carry flag output of add */
int carry_ok(u64 x, u64 y) {
    u64 s = x;
    unsigned char c;
    __asm__("addq %2, %0" : "+r"(s), "=@ccc"(c) : "r"(y));
    return 100 / (c == (s < x));
}
int carry_bad(u64 x, u64 y) {
    u64 s = x;
    unsigned char c;
    __asm__("addq %2, %0" : "+r"(s), "=@ccc"(c) : "r"(y));
    return 100 / (c == (s <= x));
}

/* 128-bit add: addq then adcq (the carry is the flag of the first instruction) */
int adc128_ok(u64 al, u64 ah, u64 bl, u64 bh) {
    u64 lo = al, hi = ah;
    __asm__("addq %2, %0; adcq %3, %1" : "+r"(lo), "+r"(hi) : "r"(bl), "r"(bh) : "cc");
    return 100 / (hi == ah + bh + (lo < al));
}
int adc128_bad(u64 al, u64 ah, u64 bl, u64 bh) {
    u64 lo = al, hi = ah;
    __asm__("addq %2, %0; adcq %3, %1" : "+r"(lo), "+r"(hi) : "r"(bl), "r"(bh) : "cc");
    return 100 / (hi == ah + bh);
}

/* bts on a register */
int bts_ok(u64 x, u64 b) {
    u64 v = x;
    unsigned char f;
    __asm__("btsq %2, %0" : "+r"(v), "=@ccc"(f) : "r"(b));
    return 100 / (int)((v >> (b & 63)) & 1);
}
int bts_bad(u64 x, u64 b) {
    u64 v = x;
    unsigned char f;
    __asm__("btsq %2, %0" : "+r"(v), "=@ccc"(f) : "r"(b));
    return 100 / (int)f; /* the old bit may be clear */
}

/* lock xadd on one memory operand */
int xadd_ok(u64 x, u64 y) {
    u64 m = x, old = y;
    __asm__ volatile("lock; xaddq %0, %1" : "+r"(old), "+m"(m) : : "memory", "cc");
    return 100 / (old == x && m == x + y);
}
int xadd_bad(u64 x, u64 y) {
    u64 m = x, old = y;
    __asm__ volatile("lock; xaddq %0, %1" : "+r"(old), "+m"(m) : : "memory", "cc");
    return 100 / (m == x);
}

/* lock cmpxchg */
int cmpxchg_ok(u64 x, u64 y, u64 n) {
    u64 m = x, prev = y;
    __asm__ volatile("lock; cmpxchgq %2, %1" : "+a"(prev), "+m"(m) : "r"(n) : "memory");
    return 100 / (x == y ? (m == n && prev == y) : (m == x && prev == x));
}
int cmpxchg_bad(u64 x, u64 y, u64 n) {
    u64 m = x, prev = y;
    __asm__ volatile("lock; cmpxchgq %2, %1" : "+a"(prev), "+m"(m) : "r"(n) : "memory");
    return 100 / (m == n);
}

/* xchg with memory */
int xchg_ok(u64 x, u64 y) {
    u64 m = x, old = y;
    __asm__ volatile("xchgq %0, %1" : "+r"(old), "+m"(m) : : "memory");
    return 100 / (old == x && m == y);
}
int xchg_bad(u64 x, u64 y) {
    u64 m = x, old = y;
    __asm__ volatile("xchgq %0, %1" : "+r"(old), "+m"(m) : : "memory");
    return 100 / (old == y);
}

/* mulq: wide product (high half in rdx) */
int mulhi_ok(u64 x) {
    u64 lo = x, hi, two = 2;
    __asm__("mulq %2" : "+a"(lo), "=d"(hi) : "rm"(two) : "cc");
    return 100 / (hi == (x >> 63) && lo == (x << 1));
}
int mulhi_bad(u64 x) {
    u64 lo = x, hi, two = 2;
    __asm__("mulq %2" : "+a"(lo), "=d"(hi) : "rm"(two) : "cc");
    return 100 / (hi == 0);
}

/* divl: edx:eax / divisor with edx = 0 */
int div_ok(u32 x, u32 y) {
    u32 q = x, r = 0, s = y | 1;
    __asm__("divl %2" : "+a"(q), "+d"(r) : "rm"(s) : "cc");
    return 100 / (q == x / s && r == x % s);
}
int div_bad(u32 x, u32 y) {
    u32 q = x, r = 0, s = y | 1;
    __asm__("divl %2" : "+a"(q), "+d"(r) : "rm"(s) : "cc");
    return 100 / (q == x);
}

/* fences are no-ops in the sequential model */
int fence_ok(u64 x) {
    u64 v = x;
    __asm__ volatile("mfence" : : : "memory");
    return 100 / (v == x);
}
int fence_bad(u64 x) {
    u64 v = x;
    __asm__ volatile("mfence" : : : "memory");
    return 100 / (v != x);
}

/* rdtsc: an arbitrary value */
int rdtsc_ok(void) {
    u32 lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return 100 / (int)(((u64)hi << 32 | lo | 1) & 1);
}
int rdtsc_bad(void) {
    u32 lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return 100 / (int)(lo & 1);
}

/* cpuid: privileged / machine specific, stays havoc under asm-privileged */
int cpuid_ok(void) {
    u32 a, d;
    __asm__ volatile("cpuid" : "=a"(a), "=d"(d) : "0"(1) : "ebx", "ecx");
    return 100 / (int)(1 + (a & 0));
}
int cpuid_bad(void) {
    u32 a, d;
    __asm__ volatile("cpuid" : "=a"(a), "=d"(d) : "0"(1) : "ebx", "ecx");
    return 100 / (int)(a & 1);
}
