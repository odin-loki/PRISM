// PRISM conformance task cxx/cxx_assume_true.cpp: expected true (no-overflow)
int cxx_assume_true(int x) {
    if (x < 0 || x > 10) return 0;
    [[assume(x <= 10)]];
    return x * 100;
}
