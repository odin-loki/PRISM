int pthread_yield(void);

void pyield_bad(void) {
    pthread_yield();
}

void pyield_ok(void) {
    if (pthread_yield() != 0)
        return;
}
