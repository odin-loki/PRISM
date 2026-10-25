// PRISM conformance task pointer/arr_ptr_true.c: expected true (memsafety)
int arr_ptr_true(int i) {
    int a[4] = {1, 2, 3, 4};
    int *p = a;
    if (i < 0 || i > 3) return 0;
    return *(p + i);
}
