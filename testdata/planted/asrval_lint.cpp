int asrval_bad(int *p) {
    auto v = p | std::views::as_rvalue;
    return v[9];
}
int asrval_ok(int *p) {
    auto v = p | std::views::as_rvalue;
    return v[0];
}
