int endian_bad(char *p) {
    return p[(int)std::endian::native];
}
int endian_ok(char *p) {
    auto e = std::endian::native;
    if (e != std::endian::little && e != std::endian::big) return 0;
    return p[0];
}
