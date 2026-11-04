// PRISM conformance task c23/c23_decimal_math_false.c: expected false (no-oob)
// every identity holds, so the index is 2, past b[2]
#include <math.h>
int c23_decimal_math_false(int s) {
    int b[2] = {0, 0};
    _Decimal64 q = quantized64(2.345DD, 0.01DD);  /* 2.34 (half-even), quantum 0.01 */
    int ok = q == 2.34DD && samequantumd64(q, 0.01DD) && sqrtd64(4.DD) == 2.DD && floord64(-1.5DD) == -2.DD;
    return b[ok ? 2 : (s & 1)];
}
