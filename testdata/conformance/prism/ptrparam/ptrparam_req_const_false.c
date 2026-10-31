// PRISM conformance task ptrparam/ptrparam_req_const_false.c: expected false (no-oob)
// write through a read-only contract (MEM-WRITE-CONST)
// requires: \valid_read(p)
void ptrparam_req_const_false(int *p) {
    p[0] = 0;
}
