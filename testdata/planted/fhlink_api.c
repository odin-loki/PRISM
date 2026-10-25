int fhlink(void *fhp, const char *to);

void fhlink_bad(void) {
    fhlink(0, "x");
}

void fhlink_ok(void) {
    if (fhlink(0, "x") != 0)
        return;
}
