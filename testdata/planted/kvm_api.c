void *kvm_open(const char *a, const char *b, const char *c, int d,
               const char *e);

void kvm_bad(void) {
    kvm_open(0, 0, 0, 0, 0);
}

void kvm_ok(void) {
    if (kvm_open(0, 0, 0, 0, 0) == 0)
        return;
}
