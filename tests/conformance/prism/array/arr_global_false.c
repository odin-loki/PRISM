// PRISM conformance task array/arr_global_false.c: expected false (no-oob)
static int table[16];
int arr_global_false(unsigned i) {
    return table[i % 17u];
}
