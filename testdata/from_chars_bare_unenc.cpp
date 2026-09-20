int from_chars_bare_unenc_bad(int n) {
    from_chars(p, p+1, n); return n;
}

int from_chars_bare_unenc_ok(int n) {
    return n;
}
