void may_throw(void);

void exc_leak_bad(void) {
    int *p = new int;
    may_throw();
    delete p;
}

void exc_leak_ok(void) {
    int *p = new int;
    delete p;
}
