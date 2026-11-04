// PRISM conformance task c23/c23_constexpr_true.c: expected true (no-oob)
constexpr int N = 4;
constexpr unsigned M = N * 2;
static_assert(M == 8);
int c23_constexpr_true(int i) {
    int a[M] = {};
    if (i < 0 || i >= M) return 0;
    return a[i];
}
