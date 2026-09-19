int rto_bad(int *p) {
    auto v = std::ranges::to<std::vector<int>>(p, p + 4);
    return v[9];
}
int rto_ok(int *p) {
    auto v = std::ranges::to<std::vector<int>>(p, p + 4);
    return v[0];
}
