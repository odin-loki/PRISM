enum { ANON_RED = 1 };

int enum_const_ok(int n) {
    (void)n;
    return ANON_RED;
}

int anon_enum_bad(int n) {
    enum { RED = 1 } e;
    (void)n;
    e = RED;
    return e;
}

int alignas_bad(int n) {
    _Alignas(8) int x = n;
    return x;
}

int alignas_ok(int n) {
    int x = n;
    return x;
}

int compound_bad(int n) {
    int x = (int){n};
    return x;
}

int compound_ok(int n) {
    int x = n;
    return x;
}
