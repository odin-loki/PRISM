int futex_waitv(void *waiters, unsigned int nr_futexes, unsigned int flags, void *timeout, int clockid);

void futexv_bad(void) {
    futex_waitv(0,0,0,0,0);
}

void futexv_ok(void) {
    if (futex_waitv(0,0,0,0,0)!=-1)
        return;
}
