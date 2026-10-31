// PRISM conformance task c99/c99_func_name_true.c: expected true (no-oob)
int c99_func_name_true(int x) {
    if (x < 0 || x >= (int)sizeof __func__) return 0;
    return __func__[x];
}
