int countl_bad(unsigned n, int *p) { return p[std::countl_zero(n)]; }
int countl_ok(unsigned n, int *p) {
    auto k = std::countl_zero(n);
    if (k > 8) return 0;
    return p[k];
}
