int mkdir(const char *path, unsigned mode);

void mkdir_bad(void) {
    mkdir("x",0755);
}

void mkdir_ok(void) {
    if (mkdir("x",0755)!=0)
        return;
}
