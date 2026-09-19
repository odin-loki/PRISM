int dropv_bad(int *p) {
    auto v = p | std::views::drop(1);
    return v[9];
}
int dropv_ok(int *p) {
    auto v = p | std::views::drop(1);
    return v[0];
}
