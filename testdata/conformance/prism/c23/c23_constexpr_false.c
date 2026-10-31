// PRISM conformance task c23/c23_constexpr_false.c: expected false (no-oob)
constexpr int N = 4;
constexpr unsigned M = N * 2;
static_assert(M == 8);
int c23_constexpr_false(int i) {
    int a[M] = {};
    if (i < 0 || i > M) return 0;
    return a[i];
}
