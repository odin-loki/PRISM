#include <stdio.h>

int fd_leak_bad(const char *path, int err) {
    FILE *fp = fopen(path, "r");
    if (err)
        return -1;
    fclose(fp);
    return 0;
}

int fd_leak_ok(const char *path, int err) {
    FILE *fp = fopen(path, "r");
    if (err) {
        fclose(fp);
        return -1;
    }
    fclose(fp);
    return 0;
}
