int asconst_unenc_bad(int n) {
    std::as_const();
    return n;
}

int asconst_unenc_ok(int n) {
    return n;
}
