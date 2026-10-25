// PRISM conformance task pointer/arr_ptr_false.c: expected false (memsafety)
int arr_ptr_false(int i) {
    int a[4] = {1, 2, 3, 4};
    int *p = a;
    if (i < 0 || i > 4) return 0;
    return *(p + i);
}
