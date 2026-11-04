// PRISM conformance task ptrparam/ptrparam_req_fixed_false.c: expected false (no-oob)
// fixed-extent contract, index 4 escapes
// requires: \valid(p + (0..3))
void ptrparam_req_fixed_false(int *p, int k) {
    if (k >= 0 && k <= 4) p[k] = 1;
}
