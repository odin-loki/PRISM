int ranges_dangle_bad(void) {
    auto v = std::string("hi") | std::views::take(1);
    return *v.begin();
}

int ranges_dangle_ok(void) {
    auto v = std::string("hi") | std::views::take(1);
    std::string out(v.begin(), v.end());
    return (int)out.size();
}
