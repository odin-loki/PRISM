void errcat_bad(void) {
    const std::error_category& c = std::generic_category();
    (void)c;
}
void errcat_ok(void) {
    const std::error_category& c = std::generic_category();
    if (c == std::system_category()) return;
    (void)c.name();
}
