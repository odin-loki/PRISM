// PRISM conformance task overflow/recursion_false.c: expected false (no-overflow)
int recursion_false(int n) {
    if (n <= 1 || n > 20) return 1;
    return n * recursion_false(n - 1);
}
