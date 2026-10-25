int pthread_sigmask(int how, const void *set, void *oldset);

void psigmask_bad(void) {
    pthread_sigmask(0, 0, 0);
}

void psigmask_ok(void) {
    if (pthread_sigmask(0, 0, 0) != 0)
        return;
}
