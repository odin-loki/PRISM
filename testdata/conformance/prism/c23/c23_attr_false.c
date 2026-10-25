// PRISM conformance task c23/c23_attr_false.c: expected false (no-overflow)
[[nodiscard]] int c23_attr_false(int a) {
    [[maybe_unused]] int z = 0;
    if (a < 0) return 0;
    return a * a;
}
