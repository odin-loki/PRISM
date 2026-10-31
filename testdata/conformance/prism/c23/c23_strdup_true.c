// PRISM conformance task c23/c23_strdup_true.c: expected true (memsafety)
#include <stdlib.h>
#include <string.h>
int c23_strdup_true(int x) {
    char *s = strdup("ab");
    if (!s) return 0;
    int r = s[x & 1];
    free(s);
    return r;
}
