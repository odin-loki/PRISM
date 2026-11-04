// PRISM conformance task cxx/cxx_ref_out_false.cpp: expected false (no-overflow)
// reference out-parameter, int input: v * 2 overflows
void cxx_ref_out_false(int& out, int v) {
    out = v * 2;
}
