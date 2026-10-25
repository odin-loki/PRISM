int cpre_unenc_bad(int n) {
    [[pre: n > 0]]
    return n;
}

int cpre_unenc_ok(int n) {
    return n;
}
