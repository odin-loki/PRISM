int widestr_unenc_bad(int n) {
    const wchar_t *p = L"hi";
    (void)p;
    return n;
}

int widestr_unenc_ok(int n) {
    return n;
}
