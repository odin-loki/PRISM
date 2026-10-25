// PRISM conformance task cxx/cxx_if_consteval_false.cpp: expected false (no-overflow)
constexpr int cxx_if_consteval_false(int a) {
    if consteval { return 0; }
    else { return a * 2; }
}
