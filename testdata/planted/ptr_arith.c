int arith_bad(int *p, int n) {
    return *(p + n);
}
int arith_ok(int *p, int n) {
    if (!p)
        return 0;
    if (n < 0)
        return 0;
    if (n >= 4)
        return 0;
    return p[n];
}
