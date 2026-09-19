int complex_bad(int n) {
    _Complex int z = n;
    (void)z;
    return n;
}

int imaginary_bad(int n) {
    _Imaginary int w = n;
    (void)w;
    return n;
}

int complex_float_bad(int n) {
    _Complex double z = n;
    (void)z;
    return n;
}

int complex_ok(int n) {
    int x = n;
    return x;
}
