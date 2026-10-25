int layoutc_bad(int *p) {
    using ok = std::is_layout_compatible<int, int>;
    return p[9];
}
int layoutc_ok(int *p) {
    using ok = std::is_layout_compatible<int, int>;
    return p[0];
}
