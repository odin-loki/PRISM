/* PIR tasks: inline asm, atomics, barriers and volatile accesses in kernel mode
 * (docs/KERNEL_MODELS.md "Inline assembly" and "Atomics and barriers"). Run with
 * `--kernel-api linux`: each use is a named assumption (`asm-havoc: "<constraints>"`,
 * `atomics-sequential: ...`, `volatile-access: ...`) and a proof is PROVED-ASSUMING.
 * Without --kernel-api all of these functions stay NEEDS-HARNESS (UNENCODED). */

/* ---- inline asm: outputs are arbitrary ---- */
int asm_out_ok(void) {
    int table[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    unsigned int v;
    __asm__ volatile("movl $1, %0" : "=r"(v));
    return table[v & 7]; /* any v, masked */
}
int asm_out_bad(void) {
    int table[4] = {1, 2, 3, 4};
    unsigned int v;
    __asm__ volatile("movl $1, %0" : "=r"(v));
    return table[v]; /* v is arbitrary: out of bounds */
}

/* ---- memory clobber: the objects its pointer operands point into change ---- */
int asm_clobber_ok(void) {
    int x = 5, y = 6;
    __asm__ volatile("" : : "r"(&x) : "memory");
    return 100 / (y - 5); /* y is not an operand: assumed unchanged (named in the assumption) */
}
int asm_clobber_bad(void) {
    int x = 5;
    __asm__ volatile("" : : "r"(&x) : "memory");
    return 100 / (x - 5); /* x may have changed: can be 0 */
}
int asm_nomem_ok(void) { /* no memory clobber, no pointer operand: x keeps its value */
    int x = 5;
    __asm__ volatile("nop" : : : "cc");
    return 100 / (x - 4);
}
int asm_indirect_out_bad(void) {
    int x = 5;
    __asm__ volatile("movl $1, %0" : "=m"(x)); /* writes x */
    return 100 / x;
}
int asm_ud2_bad(int n) { /* x86 BUG(): ud2 then unreachable */
    if (n == 5) {
        __asm__ volatile("ud2");
        __builtin_unreachable();
    }
    return n;
}
int asm_ud2_ok(int n) {
    if ((n & 3) == 5) {
        __asm__ volatile("ud2");
        __builtin_unreachable();
    }
    return n;
}

/* ---- atomics: memory orders are kept as the model's assumption, executed sequentially ---- */
int atomic_add_ok(void) {
    int v = 1;
    __atomic_fetch_add(&v, 1, __ATOMIC_SEQ_CST);
    return 100 / (v - 1); /* v == 2 */
}
int atomic_sub_bad(void) {
    int v = 1;
    __atomic_fetch_sub(&v, 1, __ATOMIC_SEQ_CST);
    return 100 / v; /* v == 0 */
}
int atomic_load_store_ok(void) {
    int v = 0;
    __atomic_store_n(&v, 4, __ATOMIC_RELEASE);
    return 100 / __atomic_load_n(&v, __ATOMIC_ACQUIRE);
}
int atomic_load_store_bad(void) {
    int v = 0;
    __atomic_store_n(&v, 4, __ATOMIC_RELEASE);
    return 100 / (__atomic_load_n(&v, __ATOMIC_ACQUIRE) - 4);
}
int cmpxchg_ok(void) { /* succeeds: v becomes 7 */
    int v = 3, expected = 3;
    _Bool done = __atomic_compare_exchange_n(&v, &expected, 7, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    return done ? 100 / v : 0;
}
int cmpxchg_fail_bad(void) { /* fails (4 != 3): v stays 3 and expected becomes 3 */
    int v = 3, expected = 4;
    _Bool done = __atomic_compare_exchange_n(&v, &expected, 7, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    return done ? 0 : 100 / (v - 3);
}
int cmpxchg_weak_bad(void) { /* a weak cmpxchg may fail even when the values are equal */
    int v = 3, expected = 3;
    _Bool done = __atomic_compare_exchange_n(&v, &expected, 7, 1, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    return 100 / (done ? 1 : 0);
}
int xchg_ok(void) {
    int v = 2;
    int old = __atomic_exchange_n(&v, 5, __ATOMIC_SEQ_CST);
    return 100 / (old + v - 6); /* 2 + 5 - 6 = 1 */
}
int fence_ok(void) {
    int v = 3;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    return 100 / v;
}

/* ---- READ_ONCE / WRITE_ONCE: volatile accesses ---- */
int volatile_ok(void) {
    int x = 1;
    *(volatile int *)&x = 4;
    return 100 / *(volatile int *)&x;
}
int volatile_bad(void) {
    int x = 1;
    *(volatile int *)&x = 4;
    return 100 / (*(volatile int *)&x - 4);
}
