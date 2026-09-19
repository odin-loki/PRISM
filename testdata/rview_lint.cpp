int rview_bad(int *p) {
    auto v = p | std::views::reverse;
    return v[9];
}
int rview_ok(int *p) {
    auto v = p | std::views::reverse;
    return v[0];
}
