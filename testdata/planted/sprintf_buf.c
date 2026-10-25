#include <stdio.h>

void sprintf_bad(const char *s) {
    char b[8];
    sprintf(b, "%s", s);
}

void sprintf_ok(const char *s) {
    char b[8];
    snprintf(b, sizeof b, "%s", s);
}
