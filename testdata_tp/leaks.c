/* The NULL/failed-open guard is not a leak; the later early return is. */
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>

int leak_after_guard(int c)
{
    char *p = malloc(8);
    if (!p)
        return -1;
    if (c)
        return 1;
    free(p);
    return 0;
}

int fd_leak_after_guard(const char *path, int c)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;
    if (c)
        return 1;
    close(fd);
    return 0;
}
