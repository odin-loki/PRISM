// PRISM conformance task cxx/cxx_auto_true.cpp: expected true (no-overflow)
auto cxx_auto_true(int a) -> int {
    auto b = a & 0xFF;
    return b * 3;
}
