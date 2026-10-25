// PRISM conformance task overflow/recursion_true.c: expected true (no-overflow)
// 12! fits in int, 13! does not
int recursion_true(int n) {
    if (n <= 1 || n > 12) return 1;
    return n * recursion_true(n - 1);
}
