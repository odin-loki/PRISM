// PRISM conformance task regress/uchar_cmp_neg_false.c: expected false (no-div0)
// regression: S4: unsigned char is promoted to int before comparing with -10
int uchar_cmp_neg_false(unsigned char c, int d) {
    if (c < -10 || c > 100) return 0;
    return 20 / d;
}
