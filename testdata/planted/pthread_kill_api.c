int pthread_kill(unsigned thread, int sig);

void pkill_bad(void) {
    pthread_kill(0, 0);
}

void pkill_ok(void) {
    if (pthread_kill(0, 0) != 0)
        return;
}
