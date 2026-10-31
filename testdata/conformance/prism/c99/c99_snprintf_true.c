// PRISM conformance task c99/c99_snprintf_true.c: expected true (no-oob)
#include <stdio.h>
int c99_snprintf_true(int x) {
    char b[4];
    int n = snprintf(b, sizeof b, "%d", x);
    return n > 0 ? b[0] : 0;
}
