// PRISM conformance task div/llong_div_false.c: expected false (no-div0)
// LLONG_MIN / -1 overflows
long long llong_div_false(long long a, long long b) {
    if (b == 0) return 0;
    return a / b;
}
