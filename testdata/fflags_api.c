char *fflagstostr(unsigned long flags);

void fflags_bad(void) {
    fflagstostr(0);
}

void fflags_ok(void) {
    if (fflagstostr(0) == 0)
        return;
}
