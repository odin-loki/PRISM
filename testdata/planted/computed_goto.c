int computed_goto_bad(int n) {
    void *p = &&lab;
    goto *p;
lab:
    return n;
}

int computed_goto_ptr_bad(int n) {
    void *p;
    goto *p;
    return n;
}

int computed_goto_ok(int n) {
    return n;
}
