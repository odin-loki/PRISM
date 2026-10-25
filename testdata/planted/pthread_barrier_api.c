int pthread_barrier_wait(void *barrier);

void pbar_bad(void) {
    pthread_barrier_wait(0);
}

void pbar_ok(void) {
    if (pthread_barrier_wait(0) != 0)
        return;
}
