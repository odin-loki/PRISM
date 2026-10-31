// PRISM conformance task c23/c23_auto_true.c: expected true (no-div0)
int c23_auto_true(int x) {
    auto d = (x & 3) + 1;
    return 12 / d;
}
