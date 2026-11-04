/* C23 <stdlib.h>/<string.h> additions in the libc models
 * (src/prism/pir/models/libc/stdlib.cpp and string.cpp): aligned_alloc,
 * free_sized, free_aligned_sized, memalignment, memccpy, memset_explicit,
 * strndup. See harness.h. Each function is one contract; `_false` ones must fail. */
#include "harness.h"
#include "../../../src/prism/pir/models/libc/string.cpp"
#include "../../../src/prism/pir/models/libc/stdlib.cpp"

/* C17 7.22.3.1: aligned_alloc with a power-of-two alignment returns NULL or n usable bytes. */
int aligned_alloc_true(int n, int j) {
    if (n < 1 || n > N || j < 0 || j >= n) return 0;
    char *p = (char *)aligned_alloc(4, (size_t)n);
    if (!p) return 0;
    p[j] = 1;
    assert(p[j] == 1);
    free(p);
    return 0;
}

/* An alignment that is not a power of two gets NULL; using it unchecked. */
int aligned_alloc_badalign_false(int n) {
    if (n < 1 || n > N) return 0;
    char *p = (char *)aligned_alloc(3, (size_t)n);
    p[0] = 1;
    free(p);
    return 0;
}

/* C23 7.24.3.4: free_sized with the size that was requested. */
int free_sized_true(int n) {
    if (n < 1 || n > N) return 0;
    char *p = (char *)malloc((size_t)n);
    free_sized(p, (size_t)n); /* NULL is allowed too */
    return 0;
}

/* A size other than the requested one is undefined. */
int free_sized_wrong_false(int n) {
    if (n < 1 || n > N) return 0;
    char *p = (char *)malloc((size_t)n);
    if (!p) return 0;
    free_sized(p, (size_t)n + 1);
    return 0;
}

/* C23 7.24.3.5: free_aligned_sized with the alignment and size of aligned_alloc. */
int free_aligned_sized_true(int n) {
    if (n < 1 || n > N) return 0;
    char *p = (char *)aligned_alloc(8, (size_t)n);
    free_aligned_sized(p, 8, (size_t)n);
    return 0;
}

/* An alignment that aligned_alloc cannot have been given (not a power of two). */
int free_aligned_sized_align_false(int n) {
    if (n < 1 || n > N) return 0;
    char *p = (char *)aligned_alloc(8, (size_t)n);
    if (!p) return 0;
    free_aligned_sized(p, 6, (size_t)n);
    return 0;
}

/* C23 7.24.3.1: memalignment is 0 for NULL, else a power of two. */
int memalignment_true(int v) {
    char b[2] = {(char)v, 0};
    assert(memalignment(0) == 0);
    size_t a = memalignment(b);
    assert(a != 0 && (a & (a - 1)) == 0);
    return 0;
}

/* Wrong contract: the alignment of an object is not a fixed 8. */
int memalignment_fixed_false(int v) {
    char b[2] = {(char)v, 0};
    assert(memalignment(b) == 8);
    return 0;
}

/* C23 7.26.2.2: memccpy stops after the first c and returns one past it in d. */
int memccpy_true(void) {
    char s[4] = {'a', 'b', 'c', 0};
    char d[4] = {0, 0, 0, 0};
    char *r = (char *)memccpy(d, s, 'b', 4);
    assert(r == d + 2);
    assert(d[0] == 'a' && d[1] == 'b');
    return 0;
}

/* No c in the first n bytes: all n are copied, past the end of a 2-byte d. */
int memccpy_oob_false(void) {
    char s[4] = {'a', 'b', 'c', 0};
    char d[2];
    memccpy(d, s, 'z', 4);
    return d[0];
}

/* C23 7.26.6.2: memset_explicit sets n bytes like memset. */
int memset_explicit_true(int c) {
    char b[N];
    memset_explicit(b, c, N);
    assert(b[0] == (char)c && b[N - 1] == (char)c);
    return 0;
}

/* One byte past the object. */
int memset_explicit_oob_false(int c) {
    char b[N];
    memset_explicit(b, c, N + 1);
    return b[0];
}

/* POSIX/C23 7.26.2.6: strndup copies at most n bytes and NUL-terminates. */
int strndup_true(int k) {
    if (k < 0 || k >= N) return 0;
    char s[N];
    MAKE_STR(s, N, k);
    char *p = strndup(s, 2);
    if (!p) return 0;
    int m = k < 2 ? k : 2;
    assert(p[m] == 0);
    if (m > 0) assert(p[0] == s[0]);
    free(p);
    return 0;
}

/* The copy holds min(strlen, n) + 1 bytes: reading past it. */
int strndup_oob_false(void) {
    char s[3] = {'a', 'b', 0};
    char *p = strndup(s, 1);
    if (!p) return 0;
    int r = p[2];
    free(p);
    return r;
}
