int stride_bad(int *p) {
    auto v = p | std::views::stride(2);
    return v[9];
}
int stride_ok(int *p) {
    auto v = p | std::views::stride(2);
    return v[0];
}
