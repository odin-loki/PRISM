int tkill(void);

void tkill_bad(void) {
    tkill();
}

void tkill_ok(void) {
    if (tkill()!=-1)
        return;
}
