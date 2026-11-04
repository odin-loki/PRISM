// PRISM conformance task c23/c23_decimal_cnotcxx_false.c: expected false (no-oob)
// Valid C, invalid C++ (implicit void* conversion, a variable named new);
// (int)3.5DF is 3, past the two-element block.
#include <stdlib.h>
int c23_decimal_cnotcxx_false(int s) {
    int *new = malloc(2 * sizeof(int));
    if (!new) return 0;
    new[0] = 0; new[1] = 0;
    _Decimal32 d = 3.5DF;
    int r = new[(int)d - 1 + (s & 0)];
    free(new);
    return r;
}
