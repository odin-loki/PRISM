typedef struct __dirstream DIR;
struct dirent;
DIR *opendir(const char *path);
struct dirent *readdir(DIR *d);

void opendir_bad(void) {
    DIR *d=opendir(".");
    readdir(d);
}

void opendir_ok(void) {
    DIR *d=opendir(".");
    if (!d)
        return;
    readdir(d);
}
