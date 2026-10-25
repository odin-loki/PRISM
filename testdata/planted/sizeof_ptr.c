void *malloc(unsigned long);

void sizeof_ptr_bad(void) {
    int *p;
    p = malloc(sizeof(p));
    (void)p;
}

void sizeof_ptr_ok(void) {
    int *p;
    p = malloc(sizeof(*p));
    (void)p;
}

void sizeof_ptr_type_ok(void) {
    int *p;
    p = malloc(sizeof(int));
    (void)p;
}
