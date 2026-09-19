int posix_spawn(void *pid, const char *path, void *fa, void *attr,
                void *argv, void *envp);

void spawn_bad(void) {
    posix_spawn(0,0,0,0,0,0);
}

void spawn_ok(void) {
    if (posix_spawn(0,0,0,0,0,0)!=0)
        return;
}
