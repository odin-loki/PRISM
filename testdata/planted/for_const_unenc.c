int for_const_unenc_bad(int n) {
    for (const int i = 0; i < n; i++) {} return n;
}

int for_const_unenc_ok(int n) {
    return n;
}
