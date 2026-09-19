int select(int nfds, void *rfds, void *wfds, void *efds, void *tv);

void select_bad(void) {
    select(0, 0, 0, 0, 0);
}

void select_ok(void) {
    if (select(0, 0, 0, 0, 0) < 0)
        return;
}
