int alignof_bad(int n) {
    (void)n;
    return (int)_Alignof(int);
}

int alignof_gnu_bad(int n) {
    (void)n;
    return (int)alignof(int);
}

int alignof_ok(int n) {
    return (int)sizeof(int);
}
