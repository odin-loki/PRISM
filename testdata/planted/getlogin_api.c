char *getlogin(void);

void getlogin_bad(void) {
    char *u=getlogin();
    (void)u[0];
}

void getlogin_ok(void) {
    char *u=getlogin();
    if (!u)
        return;
    (void)u[0];
}
