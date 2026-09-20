#include <stdio.h>
#include <stdlib.h>
void run_fread(void) {
    char buf[64];
    fread(buf, 1, 63, stdin);
    system(buf);
}
void run_strcat(void) {
    char cmd[64];
    char *p = getenv("CMD");
    strcat(cmd, p);
}
