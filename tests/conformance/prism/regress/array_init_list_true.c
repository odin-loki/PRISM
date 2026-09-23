// PRISM conformance task regress/array_init_list_true.c: expected true (no-overflow)
// regression: local arrays: aggregate initialiser items are evaluated; unlisted elements are zero
int array_init_list_true(int x) {
    int a[4] = {1, 2};
    if (x < 0 || x > 1000) return 0;
    return a[x & 3] + x;
}
