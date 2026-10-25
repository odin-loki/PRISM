int restrict_unenc_bad(int n) {
    int * restrict q = 0;
    return n + (q ? 0 : 0);
}

int restrict_ok(int n) {
    return n;
}
