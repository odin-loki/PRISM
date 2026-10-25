int execl(const char *path, const char *arg, ...);

void exec_bad(char *cmd) {
    execl(cmd, cmd, 0);
}

void exec_ok(void) {
    execl("/bin/true", "true", (char*)0);
}
