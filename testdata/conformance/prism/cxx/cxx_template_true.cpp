// PRISM conformance task cxx/cxx_template_true.cpp: expected true (no-overflow)
template <typename T> T twice(T x) { return x + x; }
int cxx_template_true(int a) {
    return static_cast<int>(twice<long long>(a));
}
