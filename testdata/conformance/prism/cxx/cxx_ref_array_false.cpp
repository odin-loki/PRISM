// PRISM conformance task cxx/cxx_ref_array_false.cpp: expected false (no-oob)
// reference to array: index 4 is past the end
int cxx_ref_array_false(int (&a)[4], int i) {
    return (i >= 0 && i <= 4) ? a[i] : 0;
}
