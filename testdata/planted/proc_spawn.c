void spawn_fork_bad(void) {
    fork();
}

void spawn_exec_bad(void) {
    execve("/bin/sh", 0, 0);
}

int spawn_ok(int n) {
    return n;
}
