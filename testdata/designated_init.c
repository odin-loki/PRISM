int desig_init_bad(int n) {
    int a[2] = {[1] = n};
    return a[0];
}

int desig_field_bad(int n) {
    struct S { int x; } s = {.x = n};
    return s.x;
}

int desig_init_ok(int n) {
    int a[2];
    a[0] = n;
    return a[0];
}
