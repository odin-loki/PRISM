// PRISM conformance task c23/c23_constexpr_ovf_true.c: expected true (no-overflow)
constexpr int LIMIT = 2147483647;
int c23_constexpr_ovf_true(int x) {
    if (x >= LIMIT) return 0;
    return x + 1;
}
