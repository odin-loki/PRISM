// PRISM conformance task array/arr_char_idx_true.c: expected true (no-oob)
int arr_char_idx_true(unsigned char c) {
    int hist[256] = {0};
    hist[c]++;
    return hist[c];
}
