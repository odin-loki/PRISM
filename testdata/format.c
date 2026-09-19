#include <stdio.h>

void fmt_bad(const char *msg) {
    printf(msg);
    fprintf(stdout, msg);
}

void fmt_ok(const char *s) {
    printf("%s", s);
    printf("hello");
}

void fmt_print(void) {
    printf("hello");
}
