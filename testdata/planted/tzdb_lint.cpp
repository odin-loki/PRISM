void tzdb_bad(void) {
    auto z = std::chrono::current_zone();
    (void)z;
}
void tzdb_ok(void) {
    auto z = std::chrono::current_zone();
    if (!z) return;
    (void)z;
}
