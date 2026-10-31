// PRISM conformance task c23/c23_strdup_false.c: expected false (memsafety)
#include <stdlib.h>
#include <string.h>
int c23_strdup_false(int x) {
    char *s = strdup("ab");
    if (!s) return 0;
    int r = s[(x & 3)];
    free(s);
    return r;
}
