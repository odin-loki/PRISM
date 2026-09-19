int issetugid(void);

void issetugid_bad(void) {
    issetugid();
}

void issetugid_ok(void) {
    if (issetugid() != 0)
        return;
}
