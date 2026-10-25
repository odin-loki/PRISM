#include <sys/stat.h>

int umask_bad(void) {
    umask(0);
    return 0;
}

int umask_ok(void) {
    umask(077);
    return 0;
}
