// PRISM conformance task ptrparam/ptrparam_req_fixed_true.c: expected true (no-oob)
// fixed-extent contract, guarded index
// requires: \valid(p + (0..3))
void ptrparam_req_fixed_true(int *p, int k) {
    if (k >= 0 && k < 4) p[k] = 1;
}
