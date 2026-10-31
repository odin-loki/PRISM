// PRISM conformance task c23/c23_decimal_cnotcxx_true.c: expected true (no-oob)
// Valid C, invalid C++: implicit void* conversion and a variable named new. The
// decimal route inserts the cast and renames the identifier.
#include <stdlib.h>
int c23_decimal_cnotcxx_true(int s) {
    int *new = malloc(2 * sizeof(int));
    if (!new) return 0;
    new[0] = 0; new[1] = 0;
    _Decimal32 d = 2.5DF;
    int r = new[(int)d - 2 + (s & 0)];
    free(new);
    return r;
}
