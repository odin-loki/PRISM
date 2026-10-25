int bpf(int cmd, void *attr, unsigned size);

void bpf_bad(void) {
    bpf(0,0,0);
}

void bpf_ok(void) {
    if (bpf(0,0,0)!=0)
        return;
}
