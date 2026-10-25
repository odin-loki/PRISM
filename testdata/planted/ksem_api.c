void *ksem_open(const char *name, int oflag);

void ksem_bad(void) {
    ksem_open("x", 0);
}

void ksem_ok(void) {
    if (ksem_open("x", 0) == 0)
        return;
}
