// PRISM conformance task c23/c23_decimal_bid_false.c: expected false (no-oob)
// The BID bits match (1.DD -> 0x31c0000000000001), so the index is 2, past b[2].
#include <string.h>
int c23_decimal_bid_false(int s) {
    int b[2] = {0, 0};
    _Decimal64 x = 1.DD;
    unsigned long long u;
    memcpy(&u, &x, sizeof u);
    return b[u == 0x31c0000000000001ULL && sizeof(_Decimal32) == 4 ? 2 : (s & 1)];
}
