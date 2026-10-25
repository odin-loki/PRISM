void format_bad(void) {
    const char *fmt;
    fmt = "x";
    std::format(fmt);
    std::print(fmt);
}

void format_ok(void) {
    std::format("{}", 1);
    std::print("hello");
}
