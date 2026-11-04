// PRISM conformance task ptrparam/ptrparam_req_const_true.c: expected true (no-oob)
// read through a read-only contract
// requires: \valid_read(p)
int ptrparam_req_const_true(const int *p) {
    return p[0];
}
