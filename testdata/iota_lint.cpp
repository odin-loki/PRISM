int iota_bad() {
    auto v = std::views::iota(0);
    return v[9];
}
int iota_ok() {
    auto v = std::views::iota(0);
    return v[0];
}
