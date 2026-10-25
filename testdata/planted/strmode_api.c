int strmode(unsigned mode, char *bp);

void strmode_bad(void) {
    strmode(0, 0);
}

void strmode_ok(void) {
    if (strmode(0, 0) != 0)
        return;
}
