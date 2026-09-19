int rfork(int flags);

void rfork_bad(void) {
    rfork(0);
}

void rfork_ok(void) {
    if (rfork(0) < 0)
        return;
}
