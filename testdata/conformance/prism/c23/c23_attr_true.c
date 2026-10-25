// PRISM conformance task c23/c23_attr_true.c: expected true (no-overflow)
[[nodiscard]] int c23_attr_true(int a) {
    [[maybe_unused]] int z = 0;
    if (a < 0 || a > 100) return 0;
    return a * a;
}
