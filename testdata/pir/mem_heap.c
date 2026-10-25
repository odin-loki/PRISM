/* PIR memory tasks: heap objects through the libc models (malloc may
 * return NULL; docs/PIR.md "Library models"). */
#include <stdlib.h>

int heap_ok(int i) {
    int *p = malloc(4 * sizeof(int));
    if (!p) return 0;
    for (int k = 0; k < 4; k++) p[k] = k;
    int r = (i >= 0 && i < 4) ? p[i] : 0;
    free(p);
    return r;
}

int heap_oob_bad(int i) {
    int *p = malloc(4 * sizeof(int));
    if (!p) return 0;
    if (i >= 0 && i <= 4) p[i] = 1; /* p[4] is past the end */
    free(p);
    return 0;
}

int null_bad(void) {
    int *p = malloc(sizeof(int));
    *p = 1; /* malloc may return NULL */
    int r = *p;
    free(p);
    return r;
}

int uaf_bad(int c) {
    int *p = malloc(sizeof(int));
    if (!p) return 0;
    *p = 5;
    free(p);
    if (c) return *p; /* use after free */
    return 0;
}

int double_free_bad(int c) {
    char *p = malloc(8);
    if (!p) return 0;
    free(p);
    if (c) free(p);
    return 1;
}

int invalid_free_bad(void) {
    int x = 3;
    int *p = &x;
    free(p); /* not heap memory */
    return 0;
}

int free_offset_bad(void) {
    char *p = malloc(8);
    if (!p) return 0;
    free(p + 1); /* not the start of the object */
    return 0;
}

int free_null_ok(void) {
    free(0);
    return 0;
}

int heap_uninit_bad(void) {
    int *p = malloc(sizeof(int));
    if (!p) return 0;
    int r = *p; /* malloc'd memory is uninitialised */
    free(p);
    return r;
}

int calloc_ok(int i) {
    int *p = calloc(4, sizeof(int));
    if (!p) return 0;
    int r = p[i & 3]; /* calloc zero-fills */
    free(p);
    return r;
}

int realloc_ok(void) {
    char *p = malloc(2);
    if (!p) return 0;
    p[0] = 'a';
    p[1] = 'b';
    char *q = realloc(p, 4);
    if (!q) {
        free(p);
        return 0;
    }
    q[3] = 'd';
    int r = q[0] + q[1];
    free(q);
    return r;
}

int realloc_stale_bad(void) {
    char *p = malloc(2);
    if (!p) return 0;
    p[0] = 'a';
    char *q = realloc(p, 4);
    if (!q) {
        free(p);
        return 0;
    }
    int r = p[0]; /* p was freed by realloc */
    free(q);
    return r;
}
