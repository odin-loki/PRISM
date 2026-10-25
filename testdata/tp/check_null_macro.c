/* A macro that does not test its parameter is not a NULL check. */
#include <stdlib.h>
#include <string.h>

#define TOUCH(ptr, ...) if ((ptr) != (void *)1) { __VA_ARGS__; }

struct expr {
    int type;
    int bound;
};

struct expr *new_expr_unchecked(int type, size_t size)
{
    struct expr *ret = malloc(size);
    TOUCH(ret);

    memset(ret, 0, size);
    ret->type = type;
    ret->bound = 0;
    return ret;
}
