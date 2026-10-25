// PRISM conformance task regress/mul_ll_true.c: expected true (no-overflow)
// regression: S4/F4: long long arithmetic is 64-bit and checked
long long mul_ll_true(long long a) {
    if (a > 1000000000LL || a < -1000000000LL) return 0;
    return a * 3;
}
