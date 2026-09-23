// PRISM conformance task overflow/postdec_true.c: expected true (no-overflow)
int postdec_true(int a) {
    if (a < -2000000000) return 0;
    a--;
    return a;
}
