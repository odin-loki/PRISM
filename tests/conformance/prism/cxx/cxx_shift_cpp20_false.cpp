// PRISM conformance task cxx/cxx_shift_cpp20_false.cpp: expected false (no-shift-ub)
int cxx_shift_cpp20_false(int s) {
    if (s < 0 || s > 32) return 0;
    return 1 << s;
}
