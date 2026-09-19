int pthread_join(unsigned t, void *ret);
int pthread_detach(unsigned t);

void join_bad(unsigned t) {
    pthread_join(t, 0);
}

void join_ok(unsigned t) {
    if (pthread_join(t, 0) != 0)
        return;
}
