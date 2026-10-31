// PRISM conformance task c23/c23_memccpy_false.c: expected false (no-oob)
#include <string.h>
int c23_memccpy_false(int n) {
    char d[4];
    const char s[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    memccpy(d, s, 0, (unsigned)(n & 7));
    return d[0];
}
