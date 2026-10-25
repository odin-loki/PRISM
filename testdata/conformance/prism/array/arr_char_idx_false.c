// PRISM conformance task array/arr_char_idx_false.c: expected false (no-oob)
int arr_char_idx_false(signed char c) {
    int hist[128] = {0};
    hist[c]++;
    return hist[0];
}
