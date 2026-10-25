int cpuset_setaffinity(int level, int which, int id,
                       unsigned long setsize, const void *mask);
int cpuset_getaffinity(int level, int which, int id,
                       unsigned long setsize, void *mask);

void cpuset_bad(void) {
    cpuset_setaffinity(0, 0, 0, 0, 0);
}

void cpuset_ok(void) {
    if (cpuset_setaffinity(0, 0, 0, 0, 0) != 0)
        return;
}
