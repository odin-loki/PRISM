/* <stdlib.h> and C++ operator new/delete operational models (docs/PIR.md
 * "Library models"). Allocation may fail (malloc/calloc/realloc return NULL
 * on some paths): code that dereferences the result without a test is a
 * real defect (CWE-690). operator new never returns NULL (it throws; the
 * throwing path leaves the function and is not followed). */
#include "prism_model.h"

void *malloc(size_t n) {
    if (n >= PRISM_MAX_OBJ || __VERIFIER_nondet_int()) return 0;
    return __prism_alloc(n, PRISM_HEAP, PRISM_UNINIT);
}

void *calloc(size_t n, size_t sz) {
    size_t t;
    if (__builtin_mul_overflow(n, sz, &t)) return 0;
    if (t >= PRISM_MAX_OBJ || __VERIFIER_nondet_int()) return 0;
    return __prism_alloc(t, PRISM_HEAP, PRISM_ZERO);
}

/* requires: p is NULL or a live pointer returned by malloc/calloc/realloc */
void free(void *p) { __prism_free(p, PRISM_HEAP); }

/* realloc(p, 0) frees p and returns NULL (glibc; undefined in C23). */
void *realloc(void *p, size_t n) {
    if (!p) return malloc(n);
    __prism_free_check(p, PRISM_HEAP);
    if (n == 0) {
        __prism_free(p, PRISM_HEAP);
        return 0;
    }
    if (n >= PRISM_MAX_OBJ || __VERIFIER_nondet_int()) return 0; /* p stays valid */
    size_t old = __prism_obj_size(p);
    void *q = __prism_alloc(n, PRISM_HEAP, PRISM_UNINIT);
    __prism_memcpy(q, p, old < n ? old : n, 0);
    __prism_free(p, PRISM_HEAP);
    return q;
}

void *_Znwm(size_t n) { /* operator new(size_t) */
    __prism_assume(n < PRISM_MAX_OBJ);
    return __prism_alloc(n, PRISM_NEW, PRISM_UNINIT);
}
void *_Znam(size_t n) { /* operator new[](size_t) */
    __prism_assume(n < PRISM_MAX_OBJ);
    return __prism_alloc(n, PRISM_NEWARR, PRISM_UNINIT);
}
void _ZdlPv(void *p) { __prism_free(p, PRISM_NEW); }          /* operator delete(void*) */
void _ZdlPvm(void *p, size_t n) { (void)n; __prism_free(p, PRISM_NEW); }
void _ZdaPv(void *p) { __prism_free(p, PRISM_NEWARR); }       /* operator delete[](void*) */
void _ZdaPvm(void *p, size_t n) { (void)n; __prism_free(p, PRISM_NEWARR); }

/* abs(INT_MIN) is undefined: the negation below is a checked `sub nsw`. */
int abs(int x) { return x < 0 ? -x : x; }
long labs(long x) { return x < 0 ? -x : x; }
long long llabs(long long x) { return x < 0 ? -x : x; }

size_t strlen(const char *s);

/* atoi/atol/strtol: the argument must be a string; the value is unknown. */
int atoi(const char *s) {
    (void)strlen(s);
    return __VERIFIER_nondet_int();
}
long atol(const char *s) {
    (void)strlen(s);
    return __VERIFIER_nondet_long();
}
long strtol(const char *s, char **end, int base) {
    size_t n = strlen(s);
    (void)base;
    if (end) {
        size_t k = __VERIFIER_nondet_ulong();
        __prism_assume(k <= n);
        *end = (char *)s + k;
    }
    return __VERIFIER_nondet_long();
}
unsigned long strtoul(const char *s, char **end, int base) {
    return (unsigned long)strtol(s, end, base);
}

int rand(void) {
    int r = __VERIFIER_nondet_int();
    __prism_assume(r >= 0);
    return r;
}
void srand(unsigned s) { (void)s; }

/* getenv: NULL or a read-only string of unknown contents */
char *getenv(const char *name) {
    (void)strlen(name);
    if (__VERIFIER_nondet_int()) return 0;
    return __prism_fresh_cstr(PRISM_CONST);
}
