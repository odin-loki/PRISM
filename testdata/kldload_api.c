int kldload(const char *name);

void kldload_bad(void) {
    kldload("mod");
}

void kldload_ok(void) {
    if (kldload("mod") < 0)
        return;
}
