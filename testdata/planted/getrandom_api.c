int getrandom(void *buf, unsigned long buflen, unsigned flags);

void getrandom_bad(void) {
    getrandom(0,0,0);
}

void getrandom_ok(void) {
    if (getrandom(0,0,0)<0)
        return;
}
