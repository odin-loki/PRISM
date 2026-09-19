int elems_bad(int *p) {
    auto v = p | std::views::elements<0>;
    return v[9];
}
int elems_ok(int *p) {
    auto v = p | std::views::elements<0>;
    return v[0];
}
