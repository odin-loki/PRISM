// PRISM conformance task macro/define_bound_true.c: expected true (no-oob)
#define N 16
int define_bound_true(int i) {
    int a[N] = {0};
    if (i < 0 || i >= N) return 0;
    return a[i];
}
