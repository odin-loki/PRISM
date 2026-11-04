// PRISM conformance task ptrparam/ptrparam_req_acsl_true.c: expected true (no-oob)
// ACSL block form
/*@ requires \valid(p + (0..n-1)); */
void ptrparam_req_acsl_true(int *p, int n) {
    if (n > 0 && n <= 4) p[n - 1] = 0;
}
