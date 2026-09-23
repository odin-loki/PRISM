// PRISM conformance task pointer/local_ptr_true.c: expected true (no-null-deref)
int local_ptr_true(int x) {
    int v = x & 255;
    int *p = &v;
    return *p + 1;
}
