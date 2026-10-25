void *valloc(unsigned n);

void valloc_bad(void) {
    valloc(8);
}

void valloc_ok(void) {
    if (valloc(8) == 0)
        return;
}
