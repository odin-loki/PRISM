int sem_wait(int *s);
int sem_post(int *s);

void sem_bad(void) {
    int s;
    sem_wait(&s);
}

void sem_ok(void) {
    int s;
    if (sem_wait(&s) != 0)
        return;
}
