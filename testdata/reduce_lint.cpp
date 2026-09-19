int reduce_bad(int *a, int *p) { return p[std::reduce(a, a + 4)]; }
int reduce_ok(int *a, int *p) {
    auto k = std::reduce(a, a + 4);
    if (k < 0 || k > 8) return 0;
    return p[k];
}
