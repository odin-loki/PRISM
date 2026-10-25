#include <stddef.h>
#include <string.h>

void memset_swap_bad(char *p) {
    memset(p, sizeof(*p), 0);
}

void memset_ok(char *p, int n) {
    memset(p, 0, n);
    memset(p, 0, sizeof(*p));
    memset(p, 1, n);
}
