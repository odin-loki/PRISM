int sem_open(const char *name, int oflag);

void semopen_bad(void) {
    sem_open("x", 0);
}

void semopen_ok(void) {
    if (sem_open("x", 0) != 0)
        return;
}
