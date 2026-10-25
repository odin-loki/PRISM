#include <stdio.h>
#include <string.h>

void copy_bad(void) {
    char buf[8];
    strcpy(buf, "overflow");
}

void cat_bad(void) {
    char buf[4];
    buf[0] = 'a';
    buf[1] = 0;
    strcat(buf, "overflow");
}

void copy_ok(void) {
    char buf[8];
    snprintf(buf, sizeof(buf), "%s", "ok");
}
