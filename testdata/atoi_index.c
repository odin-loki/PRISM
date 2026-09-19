#include <stdlib.h>

int atoi_bad(const char *s) {
    char a[4];
    int i = atoi(s);
    return a[i];
}

int atoi_ok(const char *s) {
    char a[4];
    int i = atoi(s);
    if (i < 0 || i >= 4)
        return 0;
    return a[i];
}

int atoi_return_ok(const char *s) {
    int i = atoi(s);
    return i;
}
