int to_chars_bad(void) {
    char buf[8];
    int v;
    v = 1;
    to_chars(buf, buf + 8, v);
    return buf[0];
}

int to_chars_ok(void) {
    char buf[8];
    int v;
    v = 1;
    auto r = to_chars(buf, buf + 8, v);
    if (r.ec)
        return 0;
    return buf[0];
}
