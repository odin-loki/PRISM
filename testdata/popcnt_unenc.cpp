int popcnt_unenc_bad(int n) {
    std::popcount(n);
    return n;
}

int popcnt_unenc_ok(int n) {
    return n;
}
