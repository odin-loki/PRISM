// PRISM conformance task ptrparam/ptrparam_req_acsl_false.c: expected false (no-oob)
// ACSL block form, index n is one past the contract
/*@ requires \valid(p + (0..n-1)); */
void ptrparam_req_acsl_false(int *p, int n) {
    if (n > 0 && n <= 4) p[n] = 0;
}
