/* Correct: braces inside character and string literals do not end a body,
 * so the free below stays in this function. */
#include <stdlib.h>

int count_braces(const char *s)
{
    int depth = 0;
    char *copy = malloc(2);
    if (copy == NULL)
        return -1;
    for (; *s; s++) {
        if (*s == '{')
            depth++;
        else if (*s == '}')
            depth--;
    }
    copy[0] = '}';
    copy[1] = 0;
    free(copy);
    return depth;
}
