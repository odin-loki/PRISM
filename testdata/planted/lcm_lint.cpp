int lcm_bad(int a, int b, int *p) { return p[std::lcm(a, b)]; }
int lcm_ok(int a, int b, int *p) {
    auto k = std::lcm(a, b);
    if (k < 0 || k > 8) return 0;
    return p[k];
}
