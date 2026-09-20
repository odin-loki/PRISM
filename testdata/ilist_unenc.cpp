int ilist_unenc_bad(int n) {
    initializer_list<int> x;
    (void)x;
    return n;
}

int ilist_unenc_ok(int n) {
    return n;
}
