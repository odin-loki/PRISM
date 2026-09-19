int midpt_bad(int a, int b, int *p) { return p[std::midpoint(a, b)]; }
int midpt_ok(int a, int b, int *p) {
    auto k = std::midpoint(a, b);
    if (k < 0 || k > 8) return 0;
    return p[k];
}
