int prctl(int option, ...);

void prctl_bad(void) {
    prctl(0);
}

void prctl_ok(void) {
    if (prctl(0)!=0)
        return;
}
