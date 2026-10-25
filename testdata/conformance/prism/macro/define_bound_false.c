// PRISM conformance task macro/define_bound_false.c: expected false (no-oob)
#define N 16
int define_bound_false(int i) {
    int a[N] = {0};
    if (i < 0 || i > N) return 0;
    return a[i];
}
