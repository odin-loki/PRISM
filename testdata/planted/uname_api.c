int uname(void *buf);

void uname_bad(void) {
    uname(0);
}

void uname_ok(void) {
    if (uname(0)!=0)
        return;
}
