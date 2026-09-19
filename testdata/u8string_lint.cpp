u8string_view u8_bad(void) {
    std::u8string s=u8"x";
    std::u8string_view v=s;
    return v;
}

int u8_ok(void) {
    std::u8string s=u8"x";
    std::u8string_view v=s;
    return (int)v.size();
}
