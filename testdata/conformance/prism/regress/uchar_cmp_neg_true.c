// PRISM conformance task regress/uchar_cmp_neg_true.c: expected true (no-div0)
// regression: S4: unsigned char is promoted to int before comparing with -10
int uchar_cmp_neg_true(unsigned char c) {
    if (c < -10) return 20 / (c - c);
    return c;
}
