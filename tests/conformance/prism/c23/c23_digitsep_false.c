// PRISM conformance task c23/c23_digitsep_false.c: expected false (no-overflow)
int c23_digitsep_false(int a) {
    if (a < 0 || a > 10'000) return 0;
    return a * 1'000'000;
}
