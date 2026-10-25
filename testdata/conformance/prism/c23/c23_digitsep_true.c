// PRISM conformance task c23/c23_digitsep_true.c: expected true (no-overflow)
int c23_digitsep_true(int a) {
    if (a < 0 || a > 1'000) return 0;
    return a * 1'000'000;
}
