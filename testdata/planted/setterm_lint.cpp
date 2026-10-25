void setterm_bad(void) { std::set_terminate([]{ throw 1; }); }
void setterm_ok(void) {
    auto prev = std::get_terminate();
    std::set_terminate(prev);
}
