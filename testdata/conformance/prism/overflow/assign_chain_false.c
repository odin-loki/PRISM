// PRISM conformance task overflow/assign_chain_false.c: expected false (no-overflow)
int assign_chain_false(int a) {
    int x = a & 0x7FFFFFF;
    int y = x * 4;
    int z = y * 4;
    return z * 4;
}
