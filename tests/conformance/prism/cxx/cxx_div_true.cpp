// PRISM conformance task cxx/cxx_div_true.cpp: expected true (no-overflow)
int cxx_div_true(int a, int b) {
    return b != 0 && b != -1 ? a / b : 0;
}
