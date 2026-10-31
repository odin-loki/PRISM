// PRISM conformance task ptrparam/ptrparam_req_sum_true.c: expected true (no-oob)
// contract states the extent; loop stays inside it
// requires: \valid_read(p + (0..n-1))
int ptrparam_req_sum_true(const int *p, int n) {
    int s = 0;
    for (int i = 0; i < n && i < 8; i++) s ^= p[i];
    return s;
}
