// PRISM conformance task cxx/cxx_range_for_true.cpp: expected true (no-overflow)
int cxx_range_for_true(int x) {
    int a[] = {1, 2, 3};
    int s = 0;
    if (x < 0 || x > 1000) return 0;
    for (int v : a) s += v * x;
    return s;
}
