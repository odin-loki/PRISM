int filterv_pred(int x) { return x > 0; }
int filterv_bad(int *p) {
    auto v = p | std::views::filter(filterv_pred);
    return v[9];
}
int filterv_ok(int *p) {
    auto v = p | std::views::filter(filterv_pred);
    return v[0];
}
