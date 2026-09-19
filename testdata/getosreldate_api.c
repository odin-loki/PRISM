int getosreldate(void);

void osrel_bad(void) {
    getosreldate();
}

void osrel_ok(void) {
    if (getosreldate() <= 0)
        return;
}
