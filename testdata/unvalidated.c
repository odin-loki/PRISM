#include <stdlib.h>

int unval_bad(const char *s) {
    int a[4];
    int i;
    i = atoi(s);
    return a[i];
}

int unval_ok(const char *s) {
    int a[4];
    int i;
    i = atoi(s);
    if (i < 0)
        return 0;
    if (i >= 4)
        return 0;
    return a[i];
}

int unval_div_bad(const char *s) {
    int x = 10;
    int i = atoi(s);
    return x / i;
}
