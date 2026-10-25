/* tinyexpr new_expr(): the NULL check after malloc() lives in a
 * function-like macro defined in the same file. The allocation is checked. */
#include <stdlib.h>
#include <string.h>

#define CHECK_NULL(ptr, ...) if ((ptr) == NULL) { __VA_ARGS__; return NULL; }

struct expr {
    int type;
    int bound;
};

struct expr *new_expr(int type, size_t size)
{
    struct expr *ret = malloc(size);
    CHECK_NULL(ret);

    memset(ret, 0, size);
    ret->type = type;
    ret->bound = 0;
    return ret;
}
