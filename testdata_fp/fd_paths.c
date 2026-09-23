/* Correct: the descriptor is closed on every path that opened it. */
#include <fcntl.h>
#include <unistd.h>

int read_first(const char *path, char *out)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    if (read(fd, out, 1) != 1) {
        close(fd);
        return -1;
    }
    close(fd);
    return 0;
}
