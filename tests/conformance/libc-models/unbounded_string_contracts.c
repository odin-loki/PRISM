/* Size-unbounded contract harnesses for the byte loops of the <string.h>
 * models (roadmap 8.2; docs/PIR.md "Library models verified by PRISM" and
 * "Loop invariants"). Every object is a heap object of a symbolic size
 * (below 2^40 bytes) with arbitrary contents: a string is built by
 * NEW_STR(s, n, k): n arbitrary bytes, the first k of them non-NUL (a
 * read-only loop that assumes it), a NUL at k. Positions in the contract are
 * the harness's scalar parameters, so an assert states the contract for
 * every position at once. The loops (the harness's and the model's) do not
 * close within any unwinding bound; a PROVED-UNBOUNDED verdict here comes
 * from loop invariants that PRISM proved inductive (Houdini: base and step)
 * and holds for every size in that range, not only up to N bytes.
 *
 * `_false` harnesses must be refuted with the class they plant
 * (expect_class in the .yml): wrong contracts, precondition violations
 * (unterminated strings, short destinations) and adversarial loops (reading
 * one past the terminator, off-by-one writes). */
#include "harness.h"
#include "../../../src/prism/pir/models/libc/string.c"
#include "../../../src/prism/pir/models/libc/stdlib.c"

#define BIG (1UL << 40)

#define NEW_STR(s, n, k)                                         \
    char *s = (char *)malloc(n);                                 \
    if (!s) return 0;                                            \
    __prism_havoc_bytes(s, n);                                   \
    for (size_t i_##s = 0; i_##s < (k); i_##s++)                \
        __VERIFIER_assume(s[i_##s] != 0);                        \
    s[k] = 0

#define NEW_BUF(d, n)                                            \
    char *d = (char *)malloc(n);                                 \
    if (!d) return 0

static int sgn(int v) { return (v > 0) - (v < 0); }

/* 7.24.6.3: strlen returns the number of characters before the first NUL. */
int strlen_any_true(unsigned long n, unsigned long k) {
    if (n == 0 || n >= BIG || k >= n) return 0;
    NEW_STR(s, n, k);
    size_t r = strlen(s);
    assert(r == k);
    free(s);
    return 0;
}

/* Wrong contract: the bytes before k may hold an earlier NUL (no NEW_STR). */
int strlen_any_earlier_nul_false(unsigned long n, unsigned long k) {
    if (n == 0 || n >= BIG || k >= n) return 0;
    char *s = (char *)malloc(n);
    if (!s) return 0;
    __prism_havoc_bytes(s, n);
    s[k] = 0;
    assert(strlen(s) == k);
    free(s);
    return 0;
}

/* Precondition violation: no NUL in the object (reads past its end). */
int strlen_any_unterminated_false(unsigned long n) {
    if (n == 0 || n >= BIG) return 0;
    char *s = (char *)malloc(n);
    if (!s) return 0;
    memset(s, 'a', n);
    size_t r = strlen(s);
    free(s);
    return (int)r;
}

/* Adversarial loop: a length loop that also reads the byte after the NUL. */
int strlen_any_past_nul_false(unsigned long n) {
    if (n == 0 || n >= BIG) return 0;
    NEW_STR(s, n, n - 1);
    size_t i = 0;
    while (s[i] || s[i + 1]) i++; /* s[n] is read when the NUL is the last byte */
    free(s);
    return (int)i;
}

/* POSIX strnlen: min(strlen(s), m), reading at most m bytes. */
int strnlen_any_true(unsigned long n, unsigned long k, unsigned long m) {
    if (n == 0 || n >= BIG || k >= n || m > n) return 0;
    NEW_STR(s, n, k);
    size_t r = strnlen(s, m);
    assert(r == (k < m ? k : m));
    free(s);
    return 0;
}

/* m past the end of an unterminated object. */
int strnlen_any_oob_false(unsigned long n, unsigned long m) {
    if (n == 0 || n >= BIG || m > n + 1) return 0;
    char *s = (char *)malloc(n);
    if (!s) return 0;
    memset(s, 'a', n);
    size_t r = strnlen(s, m);
    free(s);
    return (int)r;
}

/* 7.24.2.3: strcpy copies s including its NUL and returns d. */
int strcpy_any_true(unsigned long n, unsigned long k, unsigned long j) {
    if (n == 0 || n >= BIG || k >= n || j > k) return 0;
    NEW_STR(s, n, k);
    NEW_BUF(d, k + 1);
    char *r = strcpy(d, s);
    assert(r == d);
    assert(d[j] == s[j]);
    free(d);
    free(s);
    return 0;
}

/* Destination one byte short (no room for the NUL). */
int strcpy_any_short_dest_false(unsigned long n, unsigned long k) {
    if (n == 0 || n >= BIG || k >= n) return 0;
    NEW_STR(s, n, k);
    NEW_BUF(d, k);
    strcpy(d, s);
    free(d);
    free(s);
    return 0;
}

/* Adversarial loop: an off-by-one copy that writes one byte past d. */
int strcpy_any_off_by_one_false(unsigned long k) {
    if (k >= BIG) return 0;
    NEW_STR(s, k + 1, k);
    NEW_BUF(d, k + 1);
    for (size_t i = 0; i <= k + 1; i++) /* i <= k + 1: one byte too many */
        d[i] = i <= k ? s[i] : 0;
    free(d);
    free(s);
    return 0;
}

/* 7.24.2.4: strncpy writes exactly m bytes: the string, then NULs. */
int strncpy_any_true(unsigned long n, unsigned long k, unsigned long m, unsigned long j) {
    if (n == 0 || n >= BIG || k >= n || m >= BIG || j >= m) return 0;
    NEW_STR(s, n, k);
    NEW_BUF(d, m);
    char *r = strncpy(d, s, m);
    assert(r == d);
    assert(d[j] == (j < k ? s[j] : 0));
    free(d);
    free(s);
    return 0;
}

/* Wrong contract: strncpy does not NUL-terminate when strlen(s) >= m. */
int strncpy_any_terminates_false(unsigned long n, unsigned long k, unsigned long m) {
    if (n == 0 || n >= BIG || k >= n || m == 0 || m >= BIG) return 0;
    NEW_STR(s, n, k);
    NEW_BUF(d, m);
    strncpy(d, s, m);
    assert(d[m - 1] == 0);
    free(d);
    free(s);
    return 0;
}

/* 7.24.3.1: strcat appends s (with its NUL) at d's NUL. */
int strcat_any_true(unsigned long a, unsigned long b, unsigned long j) {
    if (a >= BIG || b >= BIG || j > a + b) return 0;
    NEW_STR(d, a + b + 1, a);
    NEW_STR(s, b + 1, b);
    char old = d[j < a ? j : 0];
    char *r = strcat(d, s);
    assert(r == d);
    if (j < a) assert(d[j] == old);
    else assert(d[j] == s[j - a]);
    free(s);
    free(d);
    return 0;
}

/* d has room for strlen(d) + strlen(s) bytes only (no room for the NUL). */
int strcat_any_overflow_false(unsigned long a, unsigned long b) {
    if (a >= BIG || b >= BIG) return 0;
    NEW_STR(d, a + b + 1, a);
    NEW_STR(s, b + 2, b + 1);
    strcat(d, s);
    free(s);
    free(d);
    return 0;
}

/* 7.24.3.2: strncat appends at most m characters, then a NUL. */
int strncat_any_true(unsigned long a, unsigned long b, unsigned long m, unsigned long j) {
    if (a >= BIG || b >= BIG || m >= BIG) return 0;
    unsigned long t = b < m ? b : m;
    if (j > a + t) return 0;
    NEW_STR(d, a + t + 1, a);
    NEW_STR(s, b + 1, b);
    char old = d[j < a ? j : 0];
    strncat(d, s, m);
    if (j < a) assert(d[j] == old);
    else if (j < a + t) assert(d[j] == s[j - a]);
    else assert(d[j] == 0);
    free(s);
    free(d);
    return 0;
}

/* Wrong contract: strncat always appends m characters. */
int strncat_any_count_false(unsigned long a, unsigned long b, unsigned long m) {
    if (a >= BIG || b >= BIG || m >= BIG) return 0;
    NEW_STR(d, a + m + 1, a);
    NEW_STR(s, b + 1, b);
    strncat(d, s, m);
    assert(d[a + m] == 0 && (m == 0 || d[a + m - 1] != 0));
    free(s);
    free(d);
    return 0;
}

/* 7.24.4.2 with 7.24.4p1: two strings of length k equal except at j decide by
 * the byte at j as unsigned char; equal strings compare 0; antisymmetric. */
int strcmp_any_true(unsigned long k, unsigned long j, int c) {
    if (k >= BIG || j >= k) return 0;
    NEW_STR(a, k + 1, k);
    NEW_BUF(b, k + 1);
    memcpy(b, a, k + 1);
    __VERIFIER_assume((char)c != 0);
    b[j] = (char)c;
    int r = strcmp(a, b);
    assert(sgn(r) == sgn((int)(unsigned char)a[j] - (int)(unsigned char)(char)c));
    assert(sgn(strcmp(b, a)) == -sgn(r));
    free(b);
    free(a);
    return 0;
}

/* A proper prefix sorts first. */
int strcmp_any_prefix_true(unsigned long k, int c) {
    if (k >= BIG) return 0;
    NEW_STR(a, k + 2, k);
    NEW_BUF(b, k + 2);
    memcpy(b, a, k + 1);
    __VERIFIER_assume((char)c != 0);
    b[k] = (char)c;
    b[k + 1] = 0;
    assert(strcmp(a, b) < 0 && strcmp(b, a) > 0);
    free(b);
    free(a);
    return 0;
}

/* Wrong contract: a signed-char comparison. */
int strcmp_any_signed_false(unsigned long k, unsigned long j, int c) {
    if (k >= BIG || j >= k) return 0;
    NEW_STR(a, k + 1, k);
    NEW_BUF(b, k + 1);
    memcpy(b, a, k + 1);
    __VERIFIER_assume((char)c != 0);
    b[j] = (char)c;
    int r = strcmp(a, b);
    assert(sgn(r) == sgn((int)a[j] - (int)(char)c));
    free(b);
    free(a);
    return 0;
}

/* 7.24.4.4: strncmp compares at most m characters. */
int strncmp_any_true(unsigned long k, unsigned long j, unsigned long m, int c) {
    if (k >= BIG || j >= k || m >= BIG) return 0;
    NEW_STR(a, k + 1, k);
    NEW_BUF(b, k + 1);
    memcpy(b, a, k + 1);
    __VERIFIER_assume((char)c != 0);
    b[j] = (char)c;
    int r = strncmp(a, b, m);
    if (m <= j) assert(r == 0);
    else assert(sgn(r) == sgn((int)(unsigned char)a[j] - (int)(unsigned char)(char)c));
    assert(sgn(strncmp(b, a, m)) == -sgn(r));
    free(b);
    free(a);
    return 0;
}

/* Wrong contract: strncmp ignores m. */
int strncmp_any_ignores_m_false(unsigned long k, unsigned long j, unsigned long m, int c) {
    if (k >= BIG || j >= k || m >= BIG) return 0;
    NEW_STR(a, k + 1, k);
    NEW_BUF(b, k + 1);
    memcpy(b, a, k + 1);
    __VERIFIER_assume((char)c != 0);
    b[j] = (char)c;
    assert(strncmp(a, b, m) == strcmp(a, b));
    free(b);
    free(a);
    return 0;
}

/* 7.24.5.2: strchr finds the first (char)c, the terminating NUL included. */
int strchr_any_true(unsigned long n, unsigned long k, int c, unsigned long j) {
    if (n == 0 || n >= BIG || k >= n || j > k) return 0;
    NEW_STR(s, n, k);
    char *r = strchr(s, c);
    if (r) {
        unsigned long i = (unsigned long)(r - s);
        assert(i <= k);
        assert(*r == (char)c);
        if (j < i) assert(s[j] != (char)c);
    } else {
        assert(s[j] != (char)c);
    }
    free(s);
    return 0;
}

/* Wrong contract: strchr(s, 0) is not NULL. */
int strchr_any_nul_false(unsigned long n, unsigned long k) {
    if (n == 0 || n >= BIG || k >= n) return 0;
    NEW_STR(s, n, k);
    assert(strchr(s, 0) == 0);
    free(s);
    return 0;
}

/* 7.24.5.5: strrchr finds the last (char)c. */
int strrchr_any_true(unsigned long n, unsigned long k, int c, unsigned long j) {
    if (n == 0 || n >= BIG || k >= n || j > k) return 0;
    NEW_STR(s, n, k);
    char *r = strrchr(s, c);
    if (r) {
        unsigned long i = (unsigned long)(r - s);
        assert(i <= k);
        assert(*r == (char)c);
        if (j > i) assert(s[j] != (char)c);
    } else {
        assert(s[j] != (char)c);
    }
    free(s);
    return 0;
}

/* Wrong contract: strrchr returns the first occurrence. */
int strrchr_any_first_false(unsigned long n, unsigned long k, int c) {
    if (n == 0 || n >= BIG || k >= n) return 0;
    NEW_STR(s, n, k);
    char *r = strrchr(s, c);
    char *f = strchr(s, c);
    assert(r == f);
    free(s);
    return 0;
}

/* 7.24.4.1: memcmp compares m bytes as unsigned char; a single difference
 * at j decides; no difference compares 0. */
int memcmp_any_true(unsigned long m, unsigned long j, int c) {
    if (m == 0 || m >= BIG || j >= m) return 0;
    NEW_BUF(a, m);
    __prism_havoc_bytes(a, m);
    NEW_BUF(b, m);
    memcpy(b, a, m);
    assert(memcmp(a, b, m) == 0);
    b[j] = (char)c;
    int r = memcmp(a, b, m);
    assert(sgn(r) == sgn((int)(unsigned char)a[j] - (int)(unsigned char)(char)c));
    free(b);
    free(a);
    return 0;
}

/* Wrong contract: memcmp stops at a NUL. */
int memcmp_any_nul_false(unsigned long m, unsigned long j) {
    if (m < 2 || m >= BIG || j == 0 || j >= m) return 0;
    NEW_BUF(a, m);
    __prism_havoc_bytes(a, m);
    NEW_BUF(b, m);
    memcpy(b, a, m);
    a[0] = 0;
    b[0] = 0;
    b[j] = (char)(a[j] + 1);
    assert(memcmp(a, b, m) == 0);
    free(b);
    free(a);
    return 0;
}

/* 7.24.5.1: memchr finds the first (unsigned char)c among m bytes. */
int memchr_any_true(unsigned long m, int c, unsigned long j) {
    if (m == 0 || m >= BIG || j >= m) return 0;
    NEW_BUF(s, m);
    __prism_havoc_bytes(s, m);
    char *r = (char *)memchr(s, c, m);
    if (r) {
        unsigned long i = (unsigned long)(r - s);
        assert(i < m);
        assert((unsigned char)*r == (unsigned char)c);
        if (j < i) assert((unsigned char)s[j] != (unsigned char)c);
    } else {
        assert((unsigned char)s[j] != (unsigned char)c);
    }
    free(s);
    return 0;
}

/* m past the end of the object when c is absent. */
int memchr_any_oob_false(unsigned long m) {
    if (m == 0 || m >= BIG) return 0;
    NEW_BUF(s, m);
    memset(s, 1, m);
    int r = memchr(s, 2, m + 1) != 0;
    free(s);
    return r;
}

/* POSIX strdup: NULL (allocation failure) or a fresh copy. */
int strdup_any_true(unsigned long n, unsigned long k, unsigned long j) {
    if (n == 0 || n >= BIG || k >= n || j > k) return 0;
    NEW_STR(s, n, k);
    char *p = strdup(s);
    if (p) {
        assert(p != s);
        assert(p[j] == s[j]);
        free(p);
    }
    free(s);
    return 0;
}

/* Reading the copy one byte past its NUL (the copy has strlen + 1 bytes). */
int strdup_any_past_end_false(unsigned long n, unsigned long k) {
    if (n == 0 || n >= BIG || k >= n) return 0;
    NEW_STR(s, n, k);
    char *p = strdup(s);
    int c = 0;
    if (p) {
        c = p[k + 1];
        free(p);
    }
    free(s);
    return c;
}
