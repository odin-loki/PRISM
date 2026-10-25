/* tinyexpr smoke.c:829: the copies omit the terminator, and the store
 * after the loop writes it. */
#include <string.h>

void repeat_sin(char *expr, int n)
{
    int j;
    for (j = 0; j < n; ++j)
        memcpy(expr + j * 4, "sin ", 4);
    expr[j * 4] = 0;
}

void tag_name(char *name)
{
    memcpy(name, "abc", 3);
    name[3] = '\0';
}
