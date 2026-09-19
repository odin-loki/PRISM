#include <stdlib.h>

int mem_leak_bad(int err) {
    char *p = malloc(64);
    if (err)
        return -1;
    free(p);
    return 0;
}

int mem_leak_ok(int err) {
    char *p = malloc(64);
    if (err) {
        free(p);
        return -1;
    }
    free(p);
    return 0;
}
