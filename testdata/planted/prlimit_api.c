int prlimit(int pid, int resource, void *new_limit, void *old_limit);

void prlimit_bad(void) {
    prlimit(0,0,0,0);
}

void prlimit_ok(void) {
    if (prlimit(0,0,0,0)!=0)
        return;
}
