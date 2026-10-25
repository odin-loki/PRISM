// PRISM conformance task overflow/assign_chain_true.c: expected true (no-overflow)
int assign_chain_true(int a) {
    int x = a & 1023;
    int y = x * 4;
    int z = y * 4;
    return z * 4;
}
