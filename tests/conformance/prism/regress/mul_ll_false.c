// PRISM conformance task regress/mul_ll_false.c: expected false (no-overflow)
// regression: S4/F4: long long arithmetic is 64-bit and checked
long long mul_ll_false(long long a) {
    return a * 3;
}
