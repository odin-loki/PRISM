// PRISM conformance task cxx/cxx_div_false.cpp: expected false (no-overflow)
int cxx_div_false(int a, int b) {
    return b != 0 ? a / b : 0;
}
