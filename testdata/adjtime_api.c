int adjtime(const void *delta, void *olddelta);
int ntp_adjtime(void *buf);

void adjtime_bad(void) {
    adjtime(0, 0);
}

void adjtime_ok(void) {
    if (adjtime(0, 0) != 0)
        return;
}
