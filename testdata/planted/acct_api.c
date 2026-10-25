int acct(const char *filename);

void acct_bad(void) {
    acct("x");
}

void acct_ok(void) {
    if (acct("x")!=-1)
        return;
}
