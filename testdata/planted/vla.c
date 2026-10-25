void vla_bad(int n) {
    int a[n];
    (void)a;
}

void vla_ok(void) {
    int a[4];
    (void)a;
}
