#include <fcntl.h>
#include <unistd.h>

int toctou_bad(const char *path) {
    if (access(path, 0) != 0)
        return -1;
    return open(path, 0);
}

int toctou_ok(const char *path) {
    return open(path, 0);
}
