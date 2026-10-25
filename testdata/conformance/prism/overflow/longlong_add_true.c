// PRISM conformance task overflow/longlong_add_true.c: expected true (no-overflow)
long long longlong_add_true(long long a, int b) {
    if (a > 1000000000000LL || a < -1000000000000LL) return 0;
    return a + b;
}
