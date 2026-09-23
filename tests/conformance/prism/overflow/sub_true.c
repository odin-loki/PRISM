// PRISM conformance task overflow/sub_true.c: expected true (no-overflow)
int sub_true(int a, int b) {
    if (a < 0 || b < 0) return 0;
    return a - b;
}
