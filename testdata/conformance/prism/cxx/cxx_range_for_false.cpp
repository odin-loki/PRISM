// PRISM conformance task cxx/cxx_range_for_false.cpp: expected false (no-overflow)
int cxx_range_for_false(int x) {
    int a[] = {1, 2, 3};
    int s = 0;
    if (x < 0) return 0;
    for (int v : a) s += v * x;
    return s;
}
