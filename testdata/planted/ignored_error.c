#include <stdlib.h>

void ignored_bad(void) {
    malloc(16);
}

void ignored_ok(void) {
    char *p = malloc(16);
    (void)p;
}
