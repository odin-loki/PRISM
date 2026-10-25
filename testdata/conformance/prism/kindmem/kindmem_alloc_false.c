// PRISM conformance task kindmem/kindmem_alloc_false.c: expected false (memsafety)
// k-induction with memory: a loop allocates, writes and frees a heap block each iteration
#include <stdlib.h>
int kindmem_alloc_false(int n) {
    int s = 0;
    if (n > 1000) n = 1000;
    for (int k = 0; k < n; k++) {
        char *q = malloc(2);
        if (!q) return s;
        q[k == 20 ? 2 : 1] = 1; /* past the end at k == 20 */
        s += q[1];
        free(q);
    }
    return s;
}
