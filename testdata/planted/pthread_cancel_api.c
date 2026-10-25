int pthread_cancel(unsigned thread);

void pcancel_bad(void) {
    pthread_cancel(0);
}

void pcancel_ok(void) {
    if (pthread_cancel(0) != 0)
        return;
}
