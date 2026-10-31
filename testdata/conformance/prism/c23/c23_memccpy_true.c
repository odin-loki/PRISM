// PRISM conformance task c23/c23_memccpy_true.c: expected true (no-oob)
#include <string.h>
int c23_memccpy_true(int x) {
    char d[4];
    const char s[4] = {'a', 'b', 0, 'c'};
    char *e = memccpy(d, s, 0, sizeof d);
    return e ? (int)(e - d) + (x & 1) : 0;
}
