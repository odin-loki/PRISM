/* <stdlib.h> models (src/prism/pir/models/libc/stdlib.c) against C17 7.22.
 * See harness.h. Each function is one contract; `_false` ones must fail. */
#include "harness.h"
#include "../../../src/prism/pir/models/libc/string.c" /* strlen for atoi/strtol/getenv */
#include "../../../src/prism/pir/models/libc/stdlib.c"

/* 7.22.3.4: malloc returns NULL or an object of n bytes (indeterminate). */
int malloc_true(int n, int j, int v) {
    if (n < 1 || n > N || j < 0 || j >= n) return 0;
    char *p = (char *)malloc((size_t)n);
    if (!p) return 0;
    p[j] = (char)v;
    assert(p[j] == (char)v);
    free(p);
    return 0;
}

/* One byte past the allocated object. */
int malloc_oob_false(int n) {
    if (n < 1 || n > N) return 0;
    char *p = (char *)malloc((size_t)n);
    if (!p) return 0;
    p[n] = 0;
    free(p);
    return 0;
}

/* malloc's object is uninitialised: reading it before a store. */
int malloc_uninit_false(void) {
    char *p = (char *)malloc(2);
    if (!p) return 0;
    int c = p[0];
    free(p);
    return c;
}

/* 7.22.3.2: calloc zero-fills n * size bytes; n * size overflowing is NULL. */
int calloc_true(int n, int j) {
    if (n < 1 || n > N || j < 0 || j >= 2 * n) return 0;
    short *p = (short *)calloc((size_t)n, 2 * sizeof(short));
    if (!p) return 0;
    assert(p[j] == 0);
    free(p);
    assert(calloc((size_t)-1, 2) == 0);
    return 0;
}

/* Wrong contract: calloc may fail like malloc. */
int calloc_null_false(void) {
    int *p = (int *)calloc(1, sizeof(int));
    int v = *p;
    free(p);
    return v;
}

/* 7.22.3.5: realloc keeps the first min(old, new) bytes; on failure the old
 * object is untouched. realloc(p, 0) frees p and returns NULL (glibc; the
 * model's documented choice, implementation-defined before C23). */
int realloc_true(int m, int j) {
    if (m < 1 || m > 2 * N || j < 0 || j >= N || j >= m) return 0;
    char *p = (char *)malloc(N);
    if (!p) return 0;
    for (int i = 0; i < N; i++) p[i] = (char)(i + 1);
    char *q = (char *)realloc(p, (size_t)m);
    if (!q) {
        assert(p[j] == (char)(j + 1)); /* still valid */
        free(p);
        return 0;
    }
    assert(q[j] == (char)(j + 1));
    free(q);
    return 0;
}

/* The old pointer after a successful realloc is dangling. */
int realloc_uaf_false(void) {
    char *p = (char *)malloc(2);
    if (!p) return 0;
    p[0] = 1;
    char *q = (char *)realloc(p, 4);
    if (!q) {
        free(p);
        return 0;
    }
    int c = p[0];
    free(q);
    return c;
}

/* 7.22.3.3: free(NULL) does nothing. */
int free_null_true(void) {
    free(0);
    return 0;
}

/* Double free (7.22.3.3p2). */
int free_double_false(void) {
    char *p = (char *)malloc(1);
    free(p);
    free(p);
    return 0;
}

/* Freeing a pointer that did not come from malloc. */
int free_stack_false(void) {
    char a[2];
    free(a);
    return 0;
}

/* 7.22.6.1: abs(x) = |x| when representable. */
int abs_true(int x, long y, long long z) {
    if (x == -2147483647 - 1 || y == -9223372036854775807L - 1 || z == -9223372036854775807LL - 1) return 0;
    assert(abs(x) >= 0 && (abs(x) == x || abs(x) == -x));
    assert(labs(y) >= 0 && (labs(y) == y || labs(y) == -y));
    assert(llabs(z) >= 0 && (llabs(z) == z || llabs(z) == -z));
    return 0;
}

/* abs(INT_MIN) is undefined (7.22.6.1p2). */
int abs_intmin_false(int x) { return abs(x); }

/* 7.22.1.4: strtol stores a pointer into s (at most at its NUL). */
int strtol_true(int k) {
    char s[N];
    if (k < 0 || k >= N) return 0;
    MAKE_STR(s, N, k);
    char *end = 0;
    (void)strtol(s, &end, 10);
    assert(end >= s && end - s <= k);
    (void)atoi(s);
    (void)atol(s);
    (void)strtoul(s, 0, 0);
    return 0;
}

/* atoi's argument must be a string. */
int atoi_unterminated_false(void) {
    char s[2] = {'1', '2'};
    return atoi(s);
}

/* 7.22.2.1: rand returns a value in 0..RAND_MAX (2147483647 on glibc). */
int rand_true(void) {
    srand(1);
    int r = rand();
    assert(r >= 0);
    return r;
}

/* Wrong contract: rand is not bounded by 100. */
int rand_range_false(void) {
    int r = rand();
    assert(r < 100);
    return r;
}

/* 7.22.4.6: getenv returns NULL or a string. */
int getenv_true(void) {
    char name[2] = {'P', 0};
    char *v = getenv(name);
    if (!v) return 0;
    return (int)strlen(v);
}

/* Wrong contract: getenv never returns NULL. */
int getenv_null_false(void) {
    char name[2] = {'P', 0};
    return getenv(name)[0];
}
