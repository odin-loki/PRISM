void vzip_bad(int i) {
    int a[2] = {0, 1};
    int b[3] = {0, 1, 2};
    auto z = std::views::zip(a, b);
    (void)z[i];
}
void vzip_ok(int i) {
    int a[2] = {0, 1};
    int b[3] = {0, 1, 2};
    auto z = std::views::zip(a, b);
    if (i < 0) return;
    if (i >= 2) return;
    if (i >= 3) return;
    (void)z[i];
}
