// PRISM conformance task c23/c23_static_assert_false.c: expected false (no-overflow)
int c23_static_assert_false(int a) {
    static_assert(sizeof(int) == 4);
    return a * 3;
}
