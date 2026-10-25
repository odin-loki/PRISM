int uuidgen(void *store, int count);

void uuidgen_bad(void) {
    uuidgen(0, 1);
}

void uuidgen_ok(void) {
    if (uuidgen(0, 1) != 0)
        return;
}
