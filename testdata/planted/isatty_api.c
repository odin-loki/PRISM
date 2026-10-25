int isatty(int fd);

void isatty_bad(void) {
    isatty(0);
}

void isatty_ok(void) {
    if (isatty(0)==0)
        return;
}
