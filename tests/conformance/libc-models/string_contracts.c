/* <string.h> models (src/prism/pir/models/libc/string.c) against C17 7.24.
 * See harness.h. Each function is one contract; `_false` ones must fail. */
#include "harness.h"
#include "../../../src/prism/pir/models/libc/string.c"
#include "../../../src/prism/pir/models/libc/stdlib.c" /* malloc/free for strdup */

/* 7.24.6.3: strlen returns the number of characters before the first NUL. */
int strlen_true(int k) {
    char s[N];
    if (k < 0 || k >= N) return 0;
    MAKE_STR(s, N, k);
    size_t n = strlen(s);
    assert(n == (size_t)k);
    return (int)n;
}

/* Wrong contract: the bytes before k may contain an earlier NUL. */
int strlen_earlier_nul_false(int k) {
    char s[N];
    if (k < 0 || k >= N) return 0;
    MAKE_BYTES(s, N);
    s[k] = 0;
    assert(strlen(s) == (size_t)k);
    return 0;
}

/* Precondition violation: no NUL in the object (read past the end). */
int strlen_unterminated_false(void) {
    char s[N];
    MAKE_STR(s, N, 0);
    s[0] = 'x';
    return (int)strlen(s);
}

/* POSIX strnlen: min(strlen(s), n), reading at most n bytes. */
int strnlen_true(int k, int n) {
    char s[N];
    if (k < 0 || k >= N || n < 0 || n > N) return 0;
    MAKE_STR(s, N, k);
    size_t r = strnlen(s, (size_t)n);
    assert(r == (size_t)(k < n ? k : n));
    return (int)r;
}

/* n larger than the unterminated object: reads past its end. */
int strnlen_oob_false(int n) {
    char s[N];
    if (n < 0 || n > N + 1) return 0;
    for (int i = 0; i < N; i++) s[i] = 'a';
    return (int)strnlen(s, (size_t)n);
}

/* 7.24.2.3: strcpy copies s including its NUL and returns d. */
int strcpy_true(int k, int j) {
    char s[N], d[N];
    if (k < 0 || k >= N || j < 0 || j > k) return 0;
    MAKE_STR(s, N, k);
    char *r = strcpy(d, s);
    assert(r == d);
    assert(d[j] == s[j]);
    return 0;
}

/* Destination one byte short when k == N - 1. */
int strcpy_short_dest_false(int k) {
    char s[N], d[N - 1];
    if (k < 0 || k >= N) return 0;
    MAKE_STR(s, N, k);
    strcpy(d, s);
    return d[0];
}

/* 7.24.2.4: strncpy writes exactly n bytes: the string, then NULs. */
int strncpy_true(int k, int n, int j) {
    char s[N], d[N];
    if (k < 0 || k >= N || n < 0 || n > N || j < 0 || j >= n) return 0;
    MAKE_STR(s, N, k);
    char *r = strncpy(d, s, (size_t)n);
    assert(r == d);
    assert(d[j] == (j < k ? s[j] : 0));
    return 0;
}

/* Wrong contract: strncpy does not NUL-terminate when strlen(s) >= n. */
int strncpy_terminates_false(int k, int n) {
    char s[N], d[N];
    if (k < 0 || k >= N || n < 1 || n > N) return 0;
    MAKE_STR(s, N, k);
    for (int i = 0; i < N; i++) d[i] = 'z';
    strncpy(d, s, (size_t)n);
    assert(d[n - 1] == 0);
    return 0;
}

/* 7.24.3.1: strcat appends s (with its NUL) at d's NUL. */
int strcat_true(int a, int b, int j) {
    char d[2 * N], d0[N], s[N];
    if (a < 0 || a >= N || b < 0 || b >= N || j < 0 || j >= a + b + 1) return 0;
    MAKE_STR(d0, N, a);
    MAKE_STR(s, N, b);
    for (int i = 0; i <= a; i++) d[i] = d0[i];
    char *r = strcat(d, s);
    assert(r == d);
    assert(d[j] == (j < a ? d0[j] : s[j - a]));
    return 0;
}

/* d has room for strlen(d) + 1 only. */
int strcat_overflow_false(int a) {
    char d[N], s[N];
    if (a < 0 || a >= N) return 0;
    MAKE_STR(d, N, a);
    MAKE_STR(s, N, N - 1);
    strcat(d, s);
    return d[0];
}

/* 7.24.3.2: strncat appends at most n characters, then a NUL. */
int strncat_true(int a, int b, int n, int j) {
    char d[2 * N + 1], d0[N], s[N];
    if (a < 0 || a >= N || b < 0 || b >= N || n < 0 || n > N) return 0;
    int m = b < n ? b : n;
    if (j < 0 || j > a + m) return 0;
    MAKE_STR(d0, N, a);
    MAKE_STR(s, N, b);
    for (int i = 0; i <= a; i++) d[i] = d0[i];
    strncat(d, s, (size_t)n);
    assert(d[j] == (j < a ? d0[j] : j < a + m ? s[j - a] : 0));
    return 0;
}

/* Wrong contract: strncat always appends n characters. */
int strncat_count_false(int a, int b, int n) {
    char d[2 * N + 1], s[N];
    if (a < 0 || a >= N || b < 0 || b >= N || n < 0 || n > N) return 0;
    MAKE_STR(d, N, a);
    MAKE_STR(s, N, b);
    strncat(d, s, (size_t)n);
    assert(strlen(d) == (size_t)(a + n));
    return 0;
}

static int sign(int v) { return (v > 0) - (v < 0); }

/* 7.24.4.2 with 7.24.4p1: strcmp compares as unsigned char; its sign is the
 * sign of the first difference; 0 iff the strings are equal. */
int strcmp_true(int ka, int kb, int j) {
    char a[N], b[N];
    if (ka < 0 || ka >= N || kb < 0 || kb >= N || j < 0 || j > ka) return 0;
    MAKE_STR(a, N, ka);
    MAKE_STR(b, N, kb);
    int r = strcmp(a, b);
    /* equal strings compare 0; 0 means equal (checked at every position j) */
    if (r == 0) assert(a[j] == b[j]);
    /* antisymmetry */
    assert(sign(strcmp(b, a)) == -sign(r));
    /* the first position where they differ (or a ends) decides the sign */
    int m = 0;
    while (m < ka && a[m] == b[m]) m++;
    assert(sign(r) == sign((int)(unsigned char)a[m] - (int)(unsigned char)b[m]));
    return 0;
}

/* Wrong contract: a signed-char comparison ('\xC8' sorts after '\x01'). */
int strcmp_signed_false(void) {
    char a[2] = {(char)200, 0}, b[2] = {1, 0};
    assert(strcmp(a, b) < 0);
    return 0;
}

/* 7.24.4.4: strncmp compares at most n characters. */
int strncmp_true(int ka, int kb, int n, int j) {
    char a[N], b[N];
    if (ka < 0 || ka >= N || kb < 0 || kb >= N || n < 0 || n > N) return 0;
    MAKE_STR(a, N, ka);
    MAKE_STR(b, N, kb);
    int r = strncmp(a, b, (size_t)n);
    if (n == 0) assert(r == 0);
    if (r == 0 && j >= 0 && j < n && j <= ka) assert(a[j] == b[j]);
    assert(sign(strncmp(b, a, (size_t)n)) == -sign(r));
    return 0;
}

/* Wrong contract: strncmp ignores characters past n. */
int strncmp_prefix_false(int ka, int kb) {
    char a[N], b[N];
    if (ka < 1 || ka >= N || kb < 1 || kb >= N) return 0;
    MAKE_STR(a, N, ka);
    MAKE_STR(b, N, kb);
    __VERIFIER_assume(a[0] == b[0]);
    assert(strncmp(a, b, 1) == strcmp(a, b));
    return 0;
}

/* 7.24.5.2: strchr finds the first (char)c, the terminating NUL included. */
int strchr_true(int k, int c, int j) {
    char s[N];
    if (k < 0 || k >= N || j < 0 || j > k) return 0;
    MAKE_STR(s, N, k);
    char *r = strchr(s, c);
    if (r) {
        long i = r - s;
        assert(i >= 0 && i <= k);
        assert(*r == (char)c);
        if (j < i) assert(s[j] != (char)c);
    } else {
        assert(s[j] != (char)c);
    }
    return 0;
}

/* Wrong contract: strchr(s, 0) is not NULL, it points at the terminator. */
int strchr_nul_false(int k) {
    char s[N];
    if (k < 0 || k >= N) return 0;
    MAKE_STR(s, N, k);
    assert(strchr(s, 0) == 0);
    return 0;
}

/* 7.24.5.5: strrchr finds the last (char)c. */
int strrchr_true(int k, int c, int j) {
    char s[N];
    if (k < 0 || k >= N || j < 0 || j > k) return 0;
    MAKE_STR(s, N, k);
    char *r = strrchr(s, c);
    if (r) {
        long i = r - s;
        assert(i >= 0 && i <= k);
        assert(*r == (char)c);
        if (j > i) assert(s[j] != (char)c);
    } else {
        assert(s[j] != (char)c);
    }
    return 0;
}

/* Wrong contract: strrchr returns the first occurrence. */
int strrchr_first_false(void) {
    char s[4] = {'a', 'b', 'a', 0};
    assert(strrchr(s, 'a') == s);
    return 0;
}

/* 7.24.2.1: memcpy copies n bytes; the rest of d is untouched. */
int memcpy_true(int n, int j) {
    char s[N], d[N], d0[N];
    if (n < 0 || n > N || j < 0 || j >= N) return 0;
    MAKE_BYTES(s, N);
    MAKE_BYTES(d0, N);
    for (int i = 0; i < N; i++) d[i] = d0[i];
    void *r = memcpy(d, s, (size_t)n);
    assert(r == d);
    assert(d[j] == (j < n ? s[j] : d0[j]));
    return 0;
}

/* Overlapping memcpy is undefined (7.24.2.1p2). */
int memcpy_overlap_false(int n) {
    char a[N + 1];
    if (n < 2 || n > N) return 0;
    MAKE_BYTES(a, N + 1);
    memcpy(a + 1, a, (size_t)n);
    return a[0];
}

/* 7.24.2.2: memmove copies as if through a temporary (overlap allowed). */
int memmove_true(int n, int j) {
    char a[N + 1], a0[N + 1];
    if (n < 0 || n > N || j < 1 || j > n) return 0;
    MAKE_BYTES(a0, N + 1);
    for (int i = 0; i <= N; i++) a[i] = a0[i];
    memmove(a + 1, a, (size_t)n);
    assert(a[j] == a0[j - 1]);
    assert(a[0] == a0[0]);
    return 0;
}

/* n bytes past the end of the source object. */
int memmove_oob_false(int n) {
    char a[N], b[N];
    if (n < 0 || n > N + 1) return 0;
    MAKE_BYTES(a, N);
    memmove(b, a, (size_t)n);
    return 0;
}

/* 7.24.6.1: memset stores (unsigned char)c into the first n bytes. */
int memset_true(int c, int n, int j) {
    char d[N], d0[N];
    if (n < 0 || n > N || j < 0 || j >= N) return 0;
    MAKE_BYTES(d0, N);
    for (int i = 0; i < N; i++) d[i] = d0[i];
    void *r = memset(d, c, (size_t)n);
    assert(r == d);
    assert(d[j] == (j < n ? (char)(unsigned char)c : d0[j]));
    return 0;
}

/* Wrong contract: memset stores the int, not (unsigned char)c. */
int memset_int_false(int c) {
    char d[N];
    memset(d, c, N);
    assert((int)(unsigned char)d[0] == c);
    return 0;
}

/* 7.24.4.1: memcmp compares n bytes as unsigned char. */
int memcmp_true(int n, int j) {
    char a[N], b[N];
    if (n < 0 || n > N) return 0;
    MAKE_BYTES(a, N);
    MAKE_BYTES(b, N);
    int r = memcmp(a, b, (size_t)n);
    if (r == 0 && j >= 0 && j < n) assert(a[j] == b[j]);
    int m = 0;
    while (m < n && a[m] == b[m]) m++;
    if (m == n) assert(r == 0);
    else assert(sign(r) == sign((int)(unsigned char)a[m] - (int)(unsigned char)b[m]));
    return 0;
}

/* Wrong contract: memcmp stops at a NUL like strcmp. */
int memcmp_nul_false(void) {
    char a[2] = {0, 1}, b[2] = {0, 2};
    assert(memcmp(a, b, 2) == 0);
    return 0;
}

/* 7.24.5.1: memchr finds the first (unsigned char)c among n bytes. */
int memchr_true(int c, int n, int j) {
    char s[N];
    if (n < 0 || n > N || j < 0 || j >= n) return 0;
    MAKE_BYTES(s, N);
    char *r = (char *)memchr(s, c, (size_t)n);
    if (r) {
        long i = r - s;
        assert(i >= 0 && i < n);
        assert((unsigned char)*r == (unsigned char)c);
        if (j < i) assert((unsigned char)s[j] != (unsigned char)c);
    } else {
        assert((unsigned char)s[j] != (unsigned char)c);
    }
    return 0;
}

/* n past the end of the object when c is absent. */
int memchr_oob_false(int n) {
    char s[N] = {1, 1, 1, 1};
    if (n < 0 || n > N + 1) return 0;
    return memchr(s, 2, (size_t)n) != 0;
}

/* POSIX strdup: NULL (allocation failure) or a fresh copy. */
int strdup_true(int k, int j) {
    char s[N];
    if (k < 0 || k >= N || j < 0 || j > k) return 0;
    MAKE_STR(s, N, k);
    char *p = strdup(s);
    if (!p) return 0;
    assert(p != s);
    assert(p[j] == s[j]);
    free(p);
    return 0;
}

/* Wrong contract: strdup never fails (it may return NULL). */
int strdup_null_false(void) {
    char s[2] = {'a', 0};
    char *p = strdup(s);
    int c = p[0];
    free(p);
    return c;
}
