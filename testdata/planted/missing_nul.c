void memcpy(void *d, const void *s, unsigned n);

void missing_nul_bad(void) {
    char buf[8];
    memcpy(buf, "hello", 5);
}

void missing_nul_ok(void) {
    char buf[8];
    memcpy(buf, "hello", 6);
}

void missing_nul_sizeof_ok(void) {
    char buf[8];
    memcpy(buf, "hello", sizeof("hello"));
}
