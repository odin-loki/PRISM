int exch_bad(int *p) {
    int x = 9;
    auto old = std::exchange(x, 1);
    return p[x] + old;
}
int exch_ok(int *p) {
    int x = 3;
    auto old = std::exchange(x, 1);
    if (old < 0 || old > 8) return 0;
    return p[old];
}
