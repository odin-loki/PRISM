long getdirentries(int fd, char *buf, unsigned long nbytes, long *basep);

void gde_bad(void) {
    getdirentries(0, 0, 0, 0);
}

void gde_ok(void) {
    if (getdirentries(0, 0, 0, 0) < 0)
        return;
}
