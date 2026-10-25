int getcontext(void *ucp);

void ucontext_bad(void) {
    getcontext(0);
}

void ucontext_ok(void) {
    if (getcontext(0) != 0)
        return;
}
