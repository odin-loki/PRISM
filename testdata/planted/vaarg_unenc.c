int vaarg_unenc_bad(int n) {
    int x = va_arg(ap, int);
    return x + n;
}

int vaarg_unenc_ok(int n) {
    return n;
}
