// PRISM conformance task c23/c23_nullptr_true.c: expected true (no-null-deref)
int c23_nullptr_true(int x) {
    int v = x;
    int *p = nullptr;
    if (x > 0) p = &v;
    return p ? *p : 0;
}
