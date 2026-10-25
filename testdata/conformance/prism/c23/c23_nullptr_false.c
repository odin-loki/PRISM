// PRISM conformance task c23/c23_nullptr_false.c: expected false (no-null-deref)
int c23_nullptr_false(int x) {
    int v = x;
    int *p = nullptr;
    if (x > 0) p = &v;
    return *p;
}
