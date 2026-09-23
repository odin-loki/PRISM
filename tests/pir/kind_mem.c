/* PIR tasks: k-induction for functions with memory (docs/PIR.md
 * "k-induction with memory"). The step case havocs the loop's write
 * footprint (a loop that only reads memory has an empty one); a loop that
 * allocates or frees memory is not attempted and stays BOUNDED. */
#include <stdlib.h>

static const int table[4] = {1, 2, 3, 4};

int kind_mem_read_closed(unsigned n) {
    int s = 0;
    for (unsigned i = 0; i < n; ++i) s = table[i & 3u] > 2 ? 1 : 0; /* read-only loop */
    return s;
}

int kind_mem_write_closed(unsigned n) {
    int a[4] = {0, 0, 0, 0};
    for (unsigned i = 0; i < n; ++i) a[i & 3u] = 1; /* footprint {a}, every byte initialised before */
    return a[0];
}

int kind_mem_write_uninit(unsigned n, unsigned j) {
    int a[4];
    if (n < 20) return 0;
    for (unsigned i = 0; i < n; ++i) a[i & 2u] = 1; /* a[1], a[3] stay uninitialised */
    return a[j & 3u];                               /* step open: never PROVED */
}

int kind_mem_free_bounded(unsigned n) {
    char *p = malloc(1);
    if (!p) return 0;
    for (unsigned i = 0; i < n; ++i)
        if (i == 30) { free(p); p = 0; } /* frees in the loop: not attempted */
    free(p);
    return 0;
}

int kind_mem_read_bad(unsigned n) {
    int s = 0;
    for (unsigned i = 0; i < n; ++i) s = table[i & 7u]; /* i == 4: past the end */
    return s;
}
