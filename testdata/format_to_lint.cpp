void fmtto_bad(void) {
    char buf[4];
    std::format_to(buf, "{}", 1);
}
void fmtto_ok(void) {
    char buf[4];
    std::format_to_n(buf, 3, "{}", 1);
    buf[3]=0;
}
