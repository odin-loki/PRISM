// PRISM conformance task regress/array_init_list_false.c: expected false (no-overflow)
// regression: local arrays: aggregate initialiser items are evaluated; unlisted elements are zero
int array_init_list_false(int x) {
    int a[4] = {x + 1, 2};
    return a[1];
}
