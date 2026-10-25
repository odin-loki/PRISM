// PRISM conformance task cxx/cxx_shift_cpp20_true.cpp: expected true (no-shift-ub)
// C++20 made 1<<31 well defined
int cxx_shift_cpp20_true(int s) {
    if (s < 0 || s > 31) return 0;
    return 1 << s;
}
