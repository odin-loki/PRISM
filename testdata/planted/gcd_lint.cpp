int gcd_bad(int a, int b, int *p) { return p[std::gcd(a, b)]; }
int gcd_ok(int a, int b, int *p) {
    auto k = std::gcd(a, b);
    if (k < 0 || k > 8) return 0;
    return p[k];
}
