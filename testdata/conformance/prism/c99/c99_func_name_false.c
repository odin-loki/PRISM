// PRISM conformance task c99/c99_func_name_false.c: expected false (no-oob)
int c99_func_name_false(int x) {
    if (x < 0 || x > (int)sizeof __func__) return 0;
    return __func__[x];
}
