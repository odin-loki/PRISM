int bool_bit_bad(int a, int b) {
    if (a == 1 & b == 2)
        return 1;
    return 0;
}

int bool_bit_or_bad(int x, int y) {
    if (x > 0 | y > 0)
        return 1;
    return 0;
}

int bool_bit_ok(int a, int b) {
    if (a == 1 && b == 2)
        return 1;
    return 0;
}

int bool_bit_mask_ok(unsigned flags) {
    if (flags & 0x2)
        return 1;
    return 0;
}
