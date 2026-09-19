void arch_prctl_unenc_bad(void) {
    arch_prctl();
}

int arch_prctl_unenc_ok(int n) {
    return n;
}
