int bitceil_bad(unsigned n, int *p) {
    return p[std::bit_ceil(n)];
}
int bitceil_ok(unsigned n, int *p) {
    auto k = std::bit_ceil(n);
    if (k > 8) return 0;
    return p[k];
}
