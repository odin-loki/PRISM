/* Bugs the block reachability must still see. */
#include <stdlib.h>

/* The free is in a block that falls through: the use after it is a UAF. */
int uaf_fallthrough(int c)
{
    char *p = malloc(4);
    if (!p)
        return 0;
    if (c) {
        free(p);
    }
    p[0] = 0;
    return 0;
}

/* Use after free inside the leaving block itself. */
int uaf_inside_block(int c)
{
    char *p = malloc(4);
    if (!p)
        return 0;
    if (c) {
        free(p);
        p[1] = 1;
        return 1;
    }
    free(p);
    return 0;
}

/* Freed before `break`: the code after the loop sees the freed buffer. */
int uaf_after_break(int n)
{
    char *p = malloc(4);
    if (!p)
        return 0;
    for (int i = 0; i < n; i++) {
        if (i == 3) {
            free(p);
            break;
        }
    }
    p[2] = 2;
    return 0;
}

/* A plain double free: MEM-DOUBLE-FREE only, not MEM-UAF on that line. */
void double_free_plain(void)
{
    char *p = malloc(16);
    if (!p)
        return;
    free(p);
    free(p);
}
