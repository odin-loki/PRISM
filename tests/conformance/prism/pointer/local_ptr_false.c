// PRISM conformance task pointer/local_ptr_false.c: expected false (no-null-deref)
int local_ptr_false(int x) {
    int v = x & 255;
    int *p = &v;
    if (x == 42) p = 0;
    return *p;
}
