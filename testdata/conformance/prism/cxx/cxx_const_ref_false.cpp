// PRISM conformance task cxx/cxx_const_ref_false.cpp: expected false (no-overflow)
// const reference read overflows
int cxx_const_ref_false(const int& x) {
    return x + 1;
}
