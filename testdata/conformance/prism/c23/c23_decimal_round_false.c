// PRISM conformance task c23/c23_decimal_round_false.c: expected false (no-oob)
// Under FE_DEC_UPWARD 1/3 rounds up in the last digit, so the sum exceeds 1 and the index is 2.
int c23_decimal_round_false(int s) {
    int b[2] = {0, 0};
    #pragma STDC FENV_DEC_ROUND FE_DEC_UPWARD
    _Decimal32 t = 1.DF / (_Decimal32)(s == 0 ? 3 : 3);
    _Decimal32 sum = t + t + t;
    return b[sum > 1.DF ? 2 : 0];
}
