int sstream_view_bad(void) {
    std::stringstream ss;
    std::string_view v = ss.str();
    return v[0];
}

int sstream_view_ok(void) {
    std::stringstream ss;
    std::string s = ss.str();
    if (s.size() > 0)
        return s[0];
    return 0;
}
