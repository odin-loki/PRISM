int addsat_bad(unsigned a, unsigned b, int *p) { return p[std::add_sat(a, b)]; }
int addsat_ok(unsigned a, unsigned b, int *p) {
    auto k = std::add_sat(a, b);
    if (k > 8) return 0;
    return p[k];
}
