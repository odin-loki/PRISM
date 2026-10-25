void unexpect_bad(void) {
    std::unexpected<int> u(1);
}

void unexpect_ok(void) {
    std::expected<int,int> e(1);
    if (!e.has_value()) return;
}
