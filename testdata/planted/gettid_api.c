int gettid(void);

void gettid_bad(void) {
    gettid();
}

void gettid_ok(void) {
    if (gettid()!=-1)
        return;
}
