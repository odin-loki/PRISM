int toaddr_bad(void) {
    int a[4] = {0};
    auto *p = std::to_address(a);
    return p[9];
}
int toaddr_ok(void) {
    int a[4] = {0};
    auto *p = std::to_address(a);
    return p[0];
}
