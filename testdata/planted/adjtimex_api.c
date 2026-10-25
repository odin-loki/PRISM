int adjtimex(void *tx);

void adjtimex_bad(void) {
    adjtimex(0);
}

void adjtimex_ok(void) {
    if (adjtimex(0)!=-1)
        return;
}
