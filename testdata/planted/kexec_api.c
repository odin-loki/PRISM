int kexec_load(unsigned long entry, unsigned long nr_segments, void *segments,
               unsigned long flags);

void kexec_bad(void) {
    kexec_load(0, 0, 0, 0);
}

void kexec_ok(void) {
    if (kexec_load(0, 0, 0, 0)!=-1)
        return;
}
