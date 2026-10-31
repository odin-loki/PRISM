// PRISM conformance task cxx/cxx_ref_array_true.cpp: expected true (no-oob)
// reference to array: extent known from the type
int cxx_ref_array_true(int (&a)[4], int i) {
    return (i >= 0 && i < 4) ? a[i] : 0;
}
