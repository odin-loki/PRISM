int fwdlike_bad(int *p) {
    int x = 9;
    auto y = std::forward_like<int>(x);
    return p[y];
}
int fwdlike_ok(int *p) {
    int x = 3;
    auto y = std::forward_like<int>(x);
    if (y < 0 || y > 8) return 0;
    return p[y];
}
