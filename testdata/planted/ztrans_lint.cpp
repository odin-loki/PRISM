int ztrans_bad(int *p) {
    auto v = std::views::zip_transform(std::plus<>{}, p, p);
    return v[9];
}
int ztrans_ok(int *p) {
    auto v = std::views::zip_transform(std::plus<>{}, p, p);
    return v[0];
}
