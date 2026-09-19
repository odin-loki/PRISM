int sem_timedwait(void *sem, const void *abs_timeout);

void stimed_bad(void) {
    sem_timedwait(0, 0);
}

void stimed_ok(void) {
    if (sem_timedwait(0, 0) != 0)
        return;
}
