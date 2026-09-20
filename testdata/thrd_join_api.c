int thrd_join(unsigned t, int *ret);
int thrd_detach(unsigned t);

void thrd_join_bad(unsigned t) {
    thrd_join(t, 0);
}

void thrd_join_ok(unsigned t) {
    if (thrd_join(t, 0) != 0)
        return;
}

void thrd_detach_bad(unsigned t) {
    thrd_detach(t);
}
