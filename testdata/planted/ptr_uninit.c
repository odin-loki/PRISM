int ptr_uninit_bad(void) {
    int *p;
    return *p;
}

int ptr_uninit_ok(void) {
    int x;
    int *p = &x;
    x = 1;
    return *p;
}
