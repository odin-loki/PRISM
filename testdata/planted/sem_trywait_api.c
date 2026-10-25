int sem_trywait(void *sem);
int sem_getvalue(void *sem, int *sval);

void stry_bad(void) {
    sem_trywait(0);
}

void stry_ok(void) {
    if (sem_trywait(0) != 0)
        return;
}
