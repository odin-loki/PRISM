int getdents(int fd, void *dirp, unsigned int count);

void getdents_bad(void) {
    getdents(0,0,0);
}

void getdents_ok(void) {
    if (getdents(0,0,0)!=-1)
        return;
}
