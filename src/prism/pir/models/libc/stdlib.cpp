/* <stdlib.h> and C++ operator new/delete operational models (docs/PIR.md
 * "Library models"). Allocation may fail (malloc/calloc/realloc return NULL
 * on some paths): code that dereferences the result without a test is a
 * real defect (CWE-690). A failure is recorded (__prism_alloc_failed): an
 * abort() after it is the program's out-of-memory handling, not a finding.
 * operator new never returns NULL (it throws; the throwing path leaves the
 * function and is not followed). */
#include "prism_model.h"
#include <limits.h>

extern "C" {

void *malloc(size_t n) {
    if (n >= PRISM_MAX_OBJ || __VERIFIER_nondet_int()) {
        __prism_alloc_failed();
        return 0;
    }
    return __prism_alloc(n, PRISM_HEAP, PRISM_UNINIT);
}

void *calloc(size_t n, size_t sz) {
    size_t t;
    if (__builtin_mul_overflow(n, sz, &t) || t >= PRISM_MAX_OBJ || __VERIFIER_nondet_int()) {
        __prism_alloc_failed();
        return 0;
    }
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
    if (n >= PRISM_MAX_OBJ || __VERIFIER_nondet_int()) { /* p stays valid */
        __prism_alloc_failed();
        return 0;
    }
    size_t old = __prism_obj_size(p);
    void *q = __prism_alloc(n, PRISM_HEAP, PRISM_UNINIT);
    __prism_memcpy(q, p, old < n ? old : n, 0);
    __prism_free(p, PRISM_HEAP);
    return q;
}

/* abs(INT_MIN), labs(LONG_MIN), and llabs(LLONG_MIN) are undefined.
 * The minimum is __prism_check with class FUNC-CONTRACT, then
 * __builtin_trap. It is not negated and it is not returned as a positive
 * or as zero. A negative value that is not the minimum returns the
 * negation. Zero stays zero. A positive value stays that value.
 * abs, labs, and llabs are not PROVED and not CLEAN. */
int abs(int x) {
    if (x == INT_MIN) {
        __prism_check(0, "FUNC-CONTRACT", "abs(INT_MIN) is undefined");
        __builtin_trap();
    } else if (x < 0) {
        return -x;
    } else {
        return x;
    }
}
long labs(long x) {
    if (x == LONG_MIN) {
        __prism_check(0, "FUNC-CONTRACT", "labs(LONG_MIN) is undefined");
        __builtin_trap();
    } else if (x < 0) {
        return -x;
    } else {
        return x;
    }
}
long long llabs(long long x) {
    if (x == LLONG_MIN) {
        __prism_check(0, "FUNC-CONTRACT", "llabs(LLONG_MIN) is undefined");
        __builtin_trap();
    } else if (x < 0) {
        return -x;
    } else {
        return x;
    }
}

size_t strlen(const char *s);

/* atoi/atol/strtol: the argument must be a string; the value is unknown. */
int atoi(const char *s) {
    __prism_check(s != 0, "STR-NULL-ARG", "atoi() called with unchecked pointer parameter s");
    (void)strlen(s);
    return __VERIFIER_nondet_int();
}
long atol(const char *s) {
    __prism_check(s != 0, "STR-NULL-ARG", "atol() called with unchecked pointer parameter s");
    (void)strlen(s);
    return __VERIFIER_nondet_long();
}
long strtol(const char *s, char **end, int base) {
    __prism_check(s != 0, "STR-NULL-ARG", "strtol() called with unchecked pointer parameter s");
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

/* srand and rand: the seed is not stored.
 * rand is not a reproducible sequence.
 * A run of rand is not PROVED and not CLEAN. */
int rand(void) {
    int r = __VERIFIER_nondet_int();
    __prism_assume(r >= 0);
    return r;
}
void srand(unsigned s) { (void)s; }

/* getenv: NULL or a read-only string of unknown contents */
char *getenv(const char *name) {
    __prism_check(name != 0, "STR-NULL-ARG", "getenv() called with unchecked pointer parameter name");
    (void)strlen(name);
    if (__VERIFIER_nondet_int()) return 0;
    return __prism_fresh_cstr(PRISM_CONST);
}

/* qsort does not permute the bytes and does not claim the array is sorted.
 * qsort is not PROVED and not CLEAN. */
void qsort(void *base, size_t nmemb, size_t size, int (*compar)(const void *, const void *)) {
    if (nmemb == 0 || size == 0) return;
    size_t nbytes = 0;
    if (__builtin_mul_overflow(nmemb, size, &nbytes)) {
        __prism_check(0, "MEM-OOB-READ", "qsort nmemb * size overflows");
        __prism_assume(0);
    } else {
        __prism_check(base != 0, "PTR-NULL-DEREF", "qsort() called with unchecked pointer parameter base");
        if (base == 0) return;
        __prism_check(compar != 0, "FUNC-CONTRACT", "qsort() called with unchecked pointer parameter compar");
        if (compar == 0) return;
        /* The bytes stay as they were. No order is invented. */
        (void)nbytes;
    }
}

/* A miss returns NULL. bsearch does not invent an element pointer and does
 * not read past nmemb elements. bsearch is not PROVED and not CLEAN. */
void *bsearch(const void *key, const void *base, size_t nmemb, size_t size,
              int (*compar)(const void *, const void *)) {
    if (nmemb == 0 || size == 0) return 0;
    size_t nbytes = 0;
    if (__builtin_mul_overflow(nmemb, size, &nbytes)) {
        __prism_check(0, "MEM-OOB-READ", "bsearch nmemb * size overflows");
        return 0;
    }
    __prism_check(key != 0, "PTR-NULL-DEREF", "bsearch() called with unchecked pointer parameter key");
    if (key == 0) return 0;
    __prism_check(base != 0, "PTR-NULL-DEREF", "bsearch() called with unchecked pointer parameter base");
    if (base == 0) return 0;
    __prism_check(compar != 0, "FUNC-CONTRACT", "bsearch() called with unchecked pointer parameter compar");
    if (compar == 0) return 0;
    (void)nbytes;
    /* A present key is not reported as a miss. This path is not a result. */
    __prism_assume(0);
    return 0;
}

}  // extern "C"

void *operator new(size_t n) {
    __prism_assume(n < PRISM_MAX_OBJ);
    return __prism_alloc(n, PRISM_NEW, PRISM_UNINIT);
}
void *operator new[](size_t n) {
    __prism_assume(n < PRISM_MAX_OBJ);
    return __prism_alloc(n, PRISM_NEWARR, PRISM_UNINIT);
}
void operator delete(void *p) noexcept { __prism_free(p, PRISM_NEW); }
void operator delete(void *p, size_t n) noexcept {
    (void)n;
    __prism_free(p, PRISM_NEW);
}
void operator delete[](void *p) noexcept { __prism_free(p, PRISM_NEWARR); }
void operator delete[](void *p, size_t n) noexcept {
    (void)n;
    __prism_free(p, PRISM_NEWARR);
}
