int wide_bad(int n) {
    return (int)L'A' + n;
}

int wide_ok(int n) {
    char c = 'A';
    (void)c;
    return n;
}
