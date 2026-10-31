// PRISM conformance task cxx/cxx_ref_out_true.cpp: expected true (no-overflow)
// reference out-parameter, value fits
void cxx_ref_out_true(int& out, short v) {
    out = v * 2;
}
