int enumv_bad(int *p) {
    auto v = p | std::views::enumerate;
    return v[9];
}
int enumv_ok(int *p) {
    auto v = p | std::views::enumerate;
    return v[0];
}
