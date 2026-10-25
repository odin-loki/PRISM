int construct_bad(int *p) {
    std::destroy_at(p);
    return *p;
}
int construct_ok(int *p) {
    std::construct_at(p, 1);
    return *p;
}
