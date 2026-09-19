int jwith_bad(int *p) {
    auto v = p | std::views::join_with(0);
    return v[9];
}
int jwith_ok(int *p) {
    auto v = p | std::views::join_with(0);
    return v[0];
}
