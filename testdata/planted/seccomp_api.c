int seccomp(unsigned operation, unsigned flags, void *args);

void seccomp_bad(void) {
    seccomp(0,0,0);
}

void seccomp_ok(void) {
    if (seccomp(0,0,0)!=0)
        return;
}
