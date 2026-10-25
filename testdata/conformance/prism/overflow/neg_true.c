// PRISM conformance task overflow/neg_true.c: expected true (no-overflow)
int neg_true(int a) {
    if (a == -2147483647 - 1) return 0;
    return -a;
}
