int pthread_cond_wait(void *cond, void *mutex);

void pcond_bad(void) {
    pthread_cond_wait(0, 0);
}

void pcond_ok(void) {
    if (pthread_cond_wait(0, 0) != 0)
        return;
}
