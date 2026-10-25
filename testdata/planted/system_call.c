#include <stdlib.h>

void system_bad(void) {
    char *cmd = getenv("X");
    system(cmd);
}

void system_ok(void) {
    system("true");
}
