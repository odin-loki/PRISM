int rotl_bad(unsigned n, int *p) { return p[std::rotl(n, 1)]; }
int rotl_ok(unsigned n, int *p) {
    auto k = std::rotl(n, 1);
    if (k > 8) return 0;
    return p[k];
}
