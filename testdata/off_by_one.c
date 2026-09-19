#include <string.h>

void offby_bad(const char *src) {
    char buf[4];
    strncpy(buf, src, 4);
}

void offby_ok(const char *src) {
    char buf[4];
    strncpy(buf, src, 3);
}
