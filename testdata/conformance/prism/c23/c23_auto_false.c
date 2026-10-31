// PRISM conformance task c23/c23_auto_false.c: expected false (no-div0)
int c23_auto_false(int x) {
    auto d = x & 3;
    return 12 / d;
}
