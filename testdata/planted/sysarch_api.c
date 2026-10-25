int sysarch(int number, void *args);

void sysarch_bad(void) {
    sysarch(0, 0);
}

void sysarch_ok(void) {
    if (sysarch(0, 0) != 0)
        return;
}
