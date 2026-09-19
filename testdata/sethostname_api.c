int sethostname(const char *name, unsigned len);

void sethostname_bad(void) {
    sethostname("x", 1);
}

void sethostname_ok(void) {
    if (sethostname("x", 1)!=-1)
        return;
}
