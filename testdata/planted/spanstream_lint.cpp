char *spanstream_bad(void) {
    char buf[8];
    std::spanstream ss(buf);
    return ss.span().data();
}

int spanstream_ok(void) {
    char buf[8];
    std::spanstream ss(buf);
    std::string s(ss.span().begin(), ss.span().end());
    return (int)s.size();
}
