void *login_getclass(const char *class);

void lclass_bad(void) {
    login_getclass("x");
}

void lclass_ok(void) {
    if (login_getclass("x") == 0)
        return;
}
