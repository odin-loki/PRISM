void vjoin_bad(int i) {
    int a[2] = {0, 1};
    auto j = std::views::join(a);
    (void)j[i];
}
void vjoin_ok(int i) {
    int a[2] = {0, 1};
    auto j = std::views::join(a);
    if (i < 0) return;
    if (i >= 2) return;
    (void)j[i];
}
