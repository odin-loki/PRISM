int getloginclass(char *name, unsigned long len);

void lgncls_bad(void) {
    getloginclass(0, 0);
}

void lgncls_ok(void) {
    if (getloginclass(0, 0) != 0)
        return;
}
