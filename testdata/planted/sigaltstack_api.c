int sigaltstack(const void *ss, void *old_ss);

void sigalt_bad(void) {
    sigaltstack(0, 0);
}

void sigalt_ok(void) {
    if (sigaltstack(0, 0) != 0)
        return;
}
