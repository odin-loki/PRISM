// PRISM conformance task regress/uninit_memset_false.c: expected false (memsafety)
// regression: local arrays: memset is not modelled, so a partly cleared array is never a proof
#include <string.h>
int uninit_memset_false(int i) {
    int a[4];
    memset(a, 0, 2 * sizeof(int));
    return a[i & 3];
}
