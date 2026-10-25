// PRISM conformance task c23/c23_bool_false.c: expected false (no-overflow)
int c23_bool_false(int a, bool neg) {
    return neg ? -a : a;
}
