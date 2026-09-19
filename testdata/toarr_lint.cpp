int toarr_bad(void) {
    int a[4] = {0};
    auto x = std::to_array(a);
    return x[9];
}
int toarr_ok(void) {
    int a[4] = {0};
    auto x = std::to_array(a);
    return x[0];
}
