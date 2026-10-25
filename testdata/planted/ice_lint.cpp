int ice_bad(int n, int *p) {
    if (std::is_constant_evaluated()) return 0;
    return p[n];
}
int ice_ok(int n, int *p) {
    if (std::is_constant_evaluated()) return 0;
    if (n < 0 || n > 8) return 0;
    return p[n];
}
