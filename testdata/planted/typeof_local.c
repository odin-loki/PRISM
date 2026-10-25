int typeof_bad(int n) {
    typeof(n) y = n;
    return y;
}

int typeof_gnu_bad(int n) {
    __typeof__(n) y = n;
    return y;
}

int sizeof_ok(int n) {
    return (int)sizeof(n);
}

int typeof_ok(int n) {
    int y = n;
    return y;
}
