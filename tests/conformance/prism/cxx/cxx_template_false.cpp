// PRISM conformance task cxx/cxx_template_false.cpp: expected false (no-overflow)
template <typename T> T twice(T x) { return x + x; }
int cxx_template_false(int a) {
    return twice<int>(a);
}
