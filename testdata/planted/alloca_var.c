void *alloca(unsigned long n);

void alloca_bad(int n) {
    char *p = alloca(n);
    (void)p;
}

void alloca_ok(void) {
    char *p = alloca(16);
    (void)p;
}
