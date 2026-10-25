/* Correct: realloc through a temporary; the old block is freed only on
 * the failure path, which returns. */
#include <stdlib.h>

int grow(char **buf, size_t *cap)
{
    size_t ncap = *cap ? *cap * 2 : 16;
    char *tmp = realloc(*buf, ncap);
    if (tmp == NULL) {
        free(*buf);
        *buf = NULL;
        return -1;
    }
    *buf = tmp;
    *cap = ncap;
    return 0;
}

int push(char **buf, size_t *len, size_t *cap, char c)
{
    if (*len == *cap && grow(buf, cap) != 0)
        return -1;
    (*buf)[(*len)++] = c;
    return 0;
}
