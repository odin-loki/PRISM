int countv_bad(int *p) {
    auto v = std::views::counted(p, 4);
    return v[9];
}
int countv_ok(int *p) {
    auto v = std::views::counted(p, 4);
    return v[0];
}
