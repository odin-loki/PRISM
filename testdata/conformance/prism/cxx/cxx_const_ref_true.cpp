// PRISM conformance task cxx/cxx_const_ref_true.cpp: expected true (no-overflow)
// const reference read, guarded
int cxx_const_ref_true(const int& x) {
    return x < 100 ? x + 1 : 0;
}
