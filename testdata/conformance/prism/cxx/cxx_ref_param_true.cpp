// PRISM conformance task cxx/cxx_ref_param_true.cpp: expected true (no-overflow)
// reference to a scalar: no aliasing, so a scalar model is exact
int cxx_ref_param_true(int& x) {
    if (x > 1000 || x < -1000) return 0;
    return x * 2;
}
