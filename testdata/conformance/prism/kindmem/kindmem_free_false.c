// PRISM conformance task kindmem/kindmem_free_false.c: expected false (memsafety)
// k-induction with memory: a loop frees a heap block after 20 iterations; the block is written after the loop
#include <stdlib.h>
int kindmem_free_false(int n) {
    char *p = malloc(4);
    if (!p) return 0;
    p[0] = 0;
    int freed = 0;
    if (n > 1000) n = 1000;
    for (int k = 0; k < n; k++)
        if (k == 20) { free(p); freed = 1; }
    p[0] = 1; /* use after free when n > 20 */
    if (!freed) free(p);
    return freed;
}
