int ntp_gettime(void *ntv);

void ntpgt_bad(void) {
    ntp_gettime(0);
}

void ntpgt_ok(void) {
    if (ntp_gettime(0) != 0)
        return;
}
