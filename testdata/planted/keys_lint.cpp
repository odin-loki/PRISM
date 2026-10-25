int keys_bad(int *p) {
    std::map<int,int> m;
    auto v = m | std::views::keys;
    return v[9];
}
int keys_ok(int *p) {
    std::map<int,int> m;
    auto v = m | std::views::keys;
    return v[0];
}
