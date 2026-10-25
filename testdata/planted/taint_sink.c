#include <stdlib.h>
void run(void) {
    char *cmd = getenv("CMD");
    system(cmd);
}

void ok(void) {
    system("echo hi");
}
