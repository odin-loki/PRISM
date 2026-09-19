int from_chars_bad(void) {
    char s[4];
    int v;
    from_chars(s, s + 1, v);
    return v;
}

int from_chars_ok(void) {
    char s[4];
    int v;
    auto r = from_chars(s, s + 1, v);
    if (r.ec)
        return 0;
    return v;
}
