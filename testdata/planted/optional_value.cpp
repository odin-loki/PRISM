int optional_value_bad(void) {
    std::optional<int> o;
    return o.value();
}

int optional_value_ok(void) {
    std::optional<int> o;
    if (o.has_value())
        return o.value();
    return 0;
}
