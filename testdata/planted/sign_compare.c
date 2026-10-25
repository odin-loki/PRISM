int sign_compare_bad(int i, unsigned n) {
    if (i < n)
        return 1;
    return 0;
}

int sign_compare_ok(int i, int n) {
    if (i < n)
        return 1;
    return 0;
}

int sign_compare_ok2(unsigned i, unsigned n) {
    if (i < n)
        return 1;
    return 0;
}
