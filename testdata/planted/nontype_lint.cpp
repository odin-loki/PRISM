int nontype_bad(int *p) { return p[std::nontype<9>]; }
int nontype_ok(int *p) {
    using N = decltype(std::nontype<1>);
    return p[0];
}
