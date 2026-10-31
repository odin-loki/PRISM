// PRISM conformance task c23/c23_constexpr_ovf_false.c: expected false (no-overflow)
constexpr int LIMIT = 2147483647;
int c23_constexpr_ovf_false(int x) {
    if (x < LIMIT - 1) return 0;
    return x + 1;
}
