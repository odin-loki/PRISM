/* PIR memory tasks: pointer arithmetic, comparison, lifetime, alignment,
 * read-only objects (docs/PIR.md "Memory model"). */
#include <stddef.h>

int one_past_ok(void) {
    int a[4] = {1, 2, 3, 4};
    int s = 0;
    for (int *p = a; p != a + 4; p++) s += *p; /* a + 4: one past the end is valid */
    return s;
}

int arith_bad(int k) {
    int a[4] = {1, 2, 3, 4};
    int *p = a + k; /* beyond one past the end for k > 4 */
    return p == a;
}

int arith_ok(int k) {
    int a[4] = {1, 2, 3, 4};
    if (k < 0 || k > 4) return 0;
    int *p = a + k;
    return p == a;
}

int cmp_bad(void) {
    int a[2] = {0, 0}, b[2] = {0, 0};
    return &a[0] < &b[1]; /* relational comparison across objects */
}

int cmp_ok(int i) {
    int a[4] = {0, 0, 0, 0};
    if (i < 0 || i > 3) return 0;
    return &a[i] < &a[3];
}

long diff_ok(int i) {
    char buf[16] = {0};
    if (i < 0 || i > 16) return 0;
    char *p = buf + i;
    return p - buf;
}

int *escape_bad(void) {
    int x = 3;
    int *p = &x;
    return p; /* dangling after the return */
}

static int keep = 4;
int *escape_ok(void) { return &keep; }

int null_deref_bad(int c) {
    int x = 1;
    int *p = c ? &x : NULL;
    return *p;
}

int null_deref_ok(int c) {
    int x = 1;
    int *p = c ? &x : NULL;
    return p ? *p : 0;
}

int misaligned_bad(void) {
    _Alignas(8) char buf[8] = {0};
    return *(int *)(buf + 2); /* int read at offset 2 */
}

int aligned_ok(void) {
    _Alignas(8) char buf[8] = {0};
    return *(int *)(buf + 4);
}

int literal_write_bad(void) {
    char *s = "abc";
    s[0] = 'x'; /* string literals are read-only */
    return s[0];
}

int literal_read_ok(int i) {
    const char *s = "abcd";
    return s[i & 3];
}

static int scope_helper(int **out) {
    int local = 9;
    *out = &local;
    return 0;
}

int lifetime_bad(void) {
    int *p = 0;
    scope_helper(&p);
    return *p; /* the callee's local is gone */
}

struct pair {
    int a, b;
};

static int sum_pair(struct pair p) { return p.a + p.b; }

int byval_ok(void) {
    struct pair p = {1, 2};
    return sum_pair(p);
}
