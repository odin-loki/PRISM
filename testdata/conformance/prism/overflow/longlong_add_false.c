// PRISM conformance task overflow/longlong_add_false.c: expected false (no-overflow)
long long longlong_add_false(long long a, long long b) {
    if (a < 0 || b < 0) return 0;
    return a + b;
}
