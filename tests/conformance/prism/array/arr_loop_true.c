// PRISM conformance task array/arr_loop_true.c: expected true (no-oob)
int arr_loop_true(int x) {
    int a[10];
    for (int i = 0; i < 10; i++) a[i] = x & 7;
    return a[9];
}
