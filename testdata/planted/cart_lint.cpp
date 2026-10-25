int cart_bad(int *p) {
    auto v = std::views::cartesian_product(p, p);
    return v[9];
}
int cart_ok(int *p) {
    auto v = std::views::cartesian_product(p, p);
    return v[0];
}
