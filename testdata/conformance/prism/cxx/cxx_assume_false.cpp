// PRISM conformance task cxx/cxx_assume_false.cpp: expected false (no-overflow)
int cxx_assume_false(int x) {
    if (x < 0) return 0;
    return x * 100;
}
