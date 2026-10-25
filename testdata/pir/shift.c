/* PIR tasks: shift amount and C signed left shift. */
int shl_amount_bad(int a, int s) {
    if (a < 0 || a > 1)
        return 0;
    return a << s;
}
unsigned shl_masked_ok(unsigned a, unsigned s) { return a << (s & 31u); }
int shl_sign_bad(int a) {
    if (a < 0)
        return 0;
    return a << 4;
}
int shl_sign_ok(int a) {
    if (a < 0 || a > 1000)
        return 0;
    return a << 4;
}
int ashr_ok(int a, int s) {
    if (s < 0 || s > 31)
        return 0;
    return a >> s;
}
