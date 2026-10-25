int repeat_bad() {
    auto v = std::views::repeat(1);
    return v[9];
}
int repeat_ok() {
    auto v = std::views::repeat(1);
    return v[0];
}
