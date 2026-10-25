// PRISM conformance task overflow/sub_false.c: expected false (no-overflow)
int sub_false(int a, int b) {
    if (a < 0) return 0;
    return a - b;
}
