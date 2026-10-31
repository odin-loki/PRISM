// PRISM conformance task c23/c23_strndup_true.c: expected true (memsafety)
#include <stdlib.h>
#include <string.h>
int c23_strndup_true(int x) {
    const char raw[3] = {'a', 'b', 'c'}; /* not terminated */
    char *s = strndup(raw, 3);
    if (!s) return 0;
    int r = s[3] + (x & 1);
    free(s);
    return r;
}
