int inplace_unenc_bad(int n) {
    std::inplace_vector<int, 4> v;
    (void)v;
    return n;
}

int inplace_unenc_ok(int n) {
    return n;
}
