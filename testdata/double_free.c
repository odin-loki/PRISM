#include <stdlib.h>

void double_free_bad(void) {
    char *p;
    p = malloc(16);
    free(p);
    free(p);
}

void double_free_ok(void) {
    char *p;
    p = malloc(16);
    free(p);
    p = malloc(16);
    free(p);
}
