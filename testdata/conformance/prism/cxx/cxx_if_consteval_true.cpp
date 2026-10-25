// PRISM conformance task cxx/cxx_if_consteval_true.cpp: expected true (no-overflow)
constexpr int cxx_if_consteval_true(int a) {
    if consteval { return 0; }
    else { return (a & 0xFFFF) * 2; }
}
