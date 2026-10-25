// PRISM conformance task c23/c23_typeof_false.c: expected false (no-overflow)
int c23_typeof_false(int a) {
    typeof(a) b = a & 0xFFFFF;
    return b * 10000;
}
