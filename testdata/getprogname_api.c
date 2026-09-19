const char *getprogname(void);
void setprogname(const char *progname);

void getprog_bad(void) {
    getprogname();
}

void getprog_ok(void) {
    if (getprogname() == 0)
        return;
}
