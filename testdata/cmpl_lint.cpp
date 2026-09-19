int cmpl_bad(unsigned a, int b, int *p) { return p[std::cmp_less(a, b)]; }
int cmpl_ok(unsigned a, int b, int *p) {
    if (!std::in_range<int>(a)) return 0;
    return p[0];
}
