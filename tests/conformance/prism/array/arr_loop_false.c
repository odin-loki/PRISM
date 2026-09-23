// PRISM conformance task array/arr_loop_false.c: expected false (no-oob)
// off-by-one loop writes a[10]
int arr_loop_false(int x) {
    int a[10];
    for (int i = 0; i <= 10; i++) a[i] = x & 7;
    return a[9];
}
