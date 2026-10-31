// PRISM conformance task c23/c23_decimal_printf_false.c: expected false (no-oob)
// %Df prints the exact quantum: 1.50DD -> "1.50" (4 characters), index 4 is out of b[4].
#include <stdio.h>
int c23_decimal_printf_false(int s) {
    char b[4];
    char t[32];
    int n = snprintf(t, sizeof t, "%Df", 1.50DD + (_Decimal64)(s & 0));
    return b[n];
}
