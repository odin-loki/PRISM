int adjv_bad(int *p) {
    auto v = p | std::views::adjacent<2>;
    return v[9];
}
int adjv_ok(int *p) {
    auto v = p | std::views::adjacent<2>;
    return v[0];
}
