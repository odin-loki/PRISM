// PRISM conformance task cxx/cxx_constexpr_true.cpp: expected true (no-overflow)
constexpr int cxx_constexpr_true(int a) {
    if (a < -1000 || a > 1000) return 0;
    return a * a;
}
