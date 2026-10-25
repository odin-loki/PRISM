// PRISM conformance task c23/c23_typeof_true.c: expected true (no-overflow)
int c23_typeof_true(int a) {
    typeof(a) b = a & 0xFFF;
    return b * 10000;
}
