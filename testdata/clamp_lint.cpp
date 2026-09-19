int clamp_bad(int n, int *p) { return p[std::clamp(n, 0, 100)]; }
int clamp_ok(int n, int *p) {
    auto k = std::clamp(n, 0, 8);
    if (k > 8) return 0;
    return p[k];
}
