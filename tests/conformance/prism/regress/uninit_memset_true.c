// PRISM conformance task regress/uninit_memset_true.c: expected true (memsafety)
// regression: local arrays: memset is not modelled, so a partly cleared array is never a proof
#include <string.h>
int uninit_memset_true(int i) {
    int a[4];
    memset(a, 0, sizeof a);
    return a[i & 3];
}
