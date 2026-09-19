int arch_prctl(int code, unsigned long addr);

void archpr_bad(void) {
    arch_prctl(0,0);
}

void archpr_ok(void) {
    if (arch_prctl(0,0)!=-1)
        return;
}
