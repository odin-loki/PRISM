void new_mismatch_bad(void) {
    int *p = new int[4];
    delete p;
}

void new_mismatch_ok(void) {
    int *p = new int[4];
    delete[] p;
}

void new_scalar_ok(void) {
    int *p = new int;
    delete p;
}

void new_no_ptr(void) {
    new int;
}
