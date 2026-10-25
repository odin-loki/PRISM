// PRISM conformance task cxx/cxx_auto_false.cpp: expected false (no-overflow)
auto cxx_auto_false(int a) -> int {
    auto b = a;
    return b * 3;
}
