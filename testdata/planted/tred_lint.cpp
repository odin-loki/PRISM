int tred_bad(int *a, int *p) {
    return p[std::transform_reduce(a, a + 4, 0)];
}
int tred_ok(int *a, int *p) {
    auto k = std::transform_reduce(a, a + 4, 0);
    if (k < 0 || k > 8) return 0;
    return p[k];
}
