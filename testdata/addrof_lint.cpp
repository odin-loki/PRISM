int addrof_bad(int n, int *p) { return p[*std::addressof(n)]; }
int addrof_ok(int n, int *p) {
    auto *q = std::addressof(n);
    if (!q || *q < 0 || *q > 8) return 0;
    return p[*q];
}
