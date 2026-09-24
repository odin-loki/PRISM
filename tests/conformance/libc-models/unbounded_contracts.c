/* Size-unbounded contract harnesses (roadmap 8.2). The ranged models
 * (memcpy, memmove, memset: PIR's ranged memory statements, and realloc's
 * copy) have no loop, so their contracts can be checked for objects of a
 * symbolic size n of up to 2^40 bytes (not only up to N bytes as in the
 * other harness files): a PROVED here covers every size in that range. The
 * byte loops of the string models (strlen, strcpy, ...) are checked on
 * objects of a symbolic size in unbounded_string_contracts.c, where their
 * loops close with inductive loop invariants (docs/PIR.md "Loop
 * invariants"). */
#include "harness.h"
#include "../../../src/prism/pir/models/libc/string.c"
#include "../../../src/prism/pir/models/libc/stdlib.c"

#define BIG (1UL << 40)

/* 7.24.6.1: memset stores (unsigned char)c into each of the first n bytes. */
int memset_any_size_true(unsigned long n, unsigned long j, int c) {
    if (n == 0 || n >= BIG || j >= n) return 0;
    unsigned char *p = (unsigned char *)malloc(n);
    if (!p) return 0;
    memset(p, c, n);
    assert(p[j] == (unsigned char)c);
    free(p);
    return 0;
}

/* memset one byte past a heap object of any size. */
int memset_any_size_oob_false(unsigned long n) {
    if (n == 0 || n >= BIG) return 0;
    char *p = (char *)malloc(n);
    if (!p) return 0;
    memset(p, 0, n + 1);
    free(p);
    return 0;
}

/* 7.24.2.1: memcpy copies n bytes; 7.24.2.2 memmove as through a temporary. */
int memcpy_any_size_true(unsigned long n, unsigned long j) {
    if (n == 0 || n >= BIG || j >= n) return 0;
    char *s = (char *)malloc(n), *d = (char *)malloc(n);
    if (!s || !d) {
        free(s);
        free(d);
        return 0;
    }
    memset(s, 5, n);
    s[j] = 9;
    memcpy(d, s, n);
    assert(d[j] == 9);
    memmove(s + 1, s, n - 1);
    if (j + 1 < n) assert(s[j + 1] == 9);
    free(s);
    free(d);
    return 0;
}

/* memcpy of overlapping ranges of any size. */
int memcpy_any_size_overlap_false(unsigned long n) {
    if (n < 2 || n >= BIG) return 0;
    char *s = (char *)malloc(n);
    if (!s) return 0;
    memset(s, 1, n);
    memcpy(s + 1, s, n - 1);
    free(s);
    return 0;
}

/* 7.22.3.5: realloc keeps min(old, new) bytes, for objects of any size. */
int realloc_any_size_true(unsigned long n, unsigned long m, unsigned long j) {
    if (n == 0 || m == 0 || n >= BIG || m >= BIG || j >= n || j >= m) return 0;
    char *p = (char *)malloc(n);
    if (!p) return 0;
    memset(p, 3, n);
    char *q = (char *)realloc(p, m);
    if (!q) {
        free(p);
        return 0;
    }
    assert(q[j] == 3);
    free(q);
    return 0;
}

/* Reading the grown part of a realloc'd object of any size (indeterminate). */
int realloc_any_size_uninit_false(unsigned long n) {
    if (n == 0 || n >= BIG) return 0;
    char *p = (char *)malloc(n);
    if (!p) return 0;
    memset(p, 3, n);
    char *q = (char *)realloc(p, n + 1);
    if (!q) {
        free(p);
        return 0;
    }
    int c = q[n];
    free(q);
    return c;
}
