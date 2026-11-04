// PRISM conformance task c23/c23_decimal_printf_true.c: expected true (no-oob)
// %Df prints the exact quantum: 1.50DD -> "1.50" (4 characters), index 4 is inside b[5].
#include <stdio.h>
int c23_decimal_printf_true(int s) {
    char b[5] = {0};
    char t[32];
    int n = snprintf(t, sizeof t, "%Df", 1.50DD + (_Decimal64)(s & 0));
    return (n >= 0 && n < 5) ? b[n] : 0;
}
