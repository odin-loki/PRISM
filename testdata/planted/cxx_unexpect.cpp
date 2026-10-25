int unexpect_unenc_bad(int n) {
    std::unexpected<int> u;
    return n;
}

int unexpect_unenc_ok(int n) {
    return n;
}
