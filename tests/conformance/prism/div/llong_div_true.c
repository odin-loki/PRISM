// PRISM conformance task div/llong_div_true.c: expected true (no-div0)
long long llong_div_true(long long a, long long b) {
    if (b == 0 || b == -1) return 0;
    return a / b;
}
