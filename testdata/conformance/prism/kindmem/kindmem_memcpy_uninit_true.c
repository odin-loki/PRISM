// PRISM conformance task kindmem/kindmem_memcpy_uninit_true.c: expected true (memsafety)
// k-induction with memory: a loop copies a partly uninitialised array over an initialised one (memcpy copies the initialised flags)
#include <string.h>
int kindmem_memcpy_uninit_true(int n) {
    int src[4] = {1, 2, 3, 4};
    int dst[4] = {0, 0, 0, 0};
    if (n < 20 || n > 1000) return 0;
    for (int k = 0; k < n; k++)
        if (k == 19) memcpy(dst, src, sizeof dst);
    return dst[1];
}
