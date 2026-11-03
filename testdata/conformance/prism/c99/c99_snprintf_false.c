// PRISM conformance task c99/c99_snprintf_false.c: expected false (no-oob)
#include <stdio.h>
int c99_snprintf_false(int x) {
    char b[4];
    int n = snprintf(b, sizeof b, "%d", x);
    return n > 0 ? b[n] : 0;
}
