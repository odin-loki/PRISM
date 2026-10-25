// PRISM conformance task array/arr_global_true.c: expected true (no-oob)
static int table[16];
int arr_global_true(unsigned i) {
    return table[i & 15u];
}
