void zoned_bad(void) {
    std::chrono::zoned_time zt{"bad/zone"};
    (void)zt;
}
void zoned_ok(void) {
    auto z = std::chrono::locate_zone("UTC");
    if (!z) return;
    std::chrono::zoned_time zt{z};
    (void)zt;
}
