#include <string.h>

size_t null_arg_bad(char *s) {
    return strlen(s);
}

size_t null_arg_ok(char *s) {
    if (!s)
        return 0;
    return strlen(s);
}
