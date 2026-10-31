// PRISM conformance task c23/c23_decimal_bid_true.c: expected true (no-oob)
// The object holds the IEEE 754 BID bits: 1.DD is coefficient 1, exponent 0 -> 0x31c0000000000001.
#include <string.h>
int c23_decimal_bid_true(int s) {
    int b[2] = {0, 0};
    _Decimal64 x = 1.DD;
    unsigned long long u;
    memcpy(&u, &x, sizeof u);
    return b[u == 0x31c0000000000001ULL && sizeof(_Decimal32) == 4 ? (s & 1) : 2];
}
