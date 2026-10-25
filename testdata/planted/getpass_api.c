char *getpass(const char *prompt);

void getpass_bad(void) {
    char *p=getpass("x");
    p[0]=0;
}

void getpass_ok(void) {
    char *p=getpass("x");
    if (!p)
        return;
    p[0]=0;
}
