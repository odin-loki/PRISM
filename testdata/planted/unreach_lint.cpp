int unreach_bad(int n, int *p) {
    if (n < 0) std::unreachable();
    return p[n];
}
int unreach_ok(int n, int *p) {
    if (n < 0 || n > 8) return 0;
    return p[n];
}
