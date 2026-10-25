int fork(void);
int vfork(void);
void _exit(int);

void fork_wait_bad(void) {
    fork();
}

void fork_wait_ok(void) {
    if (fork() == 0)
        _exit(0);
}

void vfork_bad(void) {
    vfork();
}
