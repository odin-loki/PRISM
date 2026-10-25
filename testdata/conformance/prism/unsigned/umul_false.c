// PRISM conformance task unsigned/umul_false.c: expected false (no-overflow)
unsigned long long umul_false(unsigned long long a, long long b) {
    return a * (unsigned long long)(b * 2);
}
