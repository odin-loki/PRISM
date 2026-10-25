int destroyn_bad(int *p) {
    std::destroy_n(p, 4);
    return p[0];
}
int destroyn_ok(int *p) {
    std::destroy_n(p, 4);
    return 0;
}
