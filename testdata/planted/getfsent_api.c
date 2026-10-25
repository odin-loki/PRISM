void *getfsent(void);

void fsent_bad(void) {
    getfsent();
}

void fsent_ok(void) {
    if (getfsent() == 0)
        return;
}
