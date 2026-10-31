// PRISM conformance task ptrparam/ptrparam_req_sum_false.c: expected false (no-oob)
// off-by-one read past the stated extent
// requires: \valid_read(p + (0..n-1))
int ptrparam_req_sum_false(const int *p, int n) {
    int s = 0;
    for (int i = 0; i <= n && i < 8; i++) s ^= p[i];
    return s;
}
