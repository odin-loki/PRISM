int pthread_atfork(void *prepare, void *parent, void *child);

void patfork_bad(void) {
    pthread_atfork(0, 0, 0);
}

void patfork_ok(void) {
    if (pthread_atfork(0, 0, 0) != 0)
        return;
}
