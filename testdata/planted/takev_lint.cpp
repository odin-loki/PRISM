int takev_bad(int *p) {
    auto v = p | std::views::take(4);
    return v[9];
}
int takev_ok(int *p) {
    auto v = p | std::views::take(4);
    return v[0];
}
