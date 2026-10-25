// PRISM conformance task c23/c23_static_assert_true.c: expected true (no-overflow)
int c23_static_assert_true(int a) {
    static_assert(sizeof(int) == 4);
    if (a > 100 || a < -100) return 0;
    return a * 3;
}
