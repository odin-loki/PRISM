int kldfirstmod(int fileid);

void kldfm_bad(void) {
    kldfirstmod(0);
}

void kldfm_ok(void) {
    if (kldfirstmod(0) < 0)
        return;
}
