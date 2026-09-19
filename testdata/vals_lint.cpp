int vals_bad(int *p) {
    std::map<int,int> m;
    auto v = m | std::views::values;
    return v[9];
}
int vals_ok(int *p) {
    std::map<int,int> m;
    auto v = m | std::views::values;
    return v[0];
}
