int bitw_bad(unsigned n, int *p) { return p[std::bit_width(n)]; }
int bitw_ok(unsigned n, int *p) {
    auto k = std::bit_width(n);
    if (k > 8) return 0;
    return p[k];
}
