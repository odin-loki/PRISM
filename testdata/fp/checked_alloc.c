/* Correct: every allocation is tested before it is used. */
#include <stdlib.h>
#include <string.h>

int checked_two_lines(void)
{
    int *p = malloc(sizeof *p);
    if (p == NULL)
        return -1;
    *p = 3;
    free(p);
    return 0;
}

int checked_one_line(void)
{
    int *p = malloc(sizeof *p); if (!p) return -1; *p = 3;
    free(p);
    return 0;
}

int checked_in_condition(size_t n)
{
    char *buf;
    if ((buf = malloc(n + 1)) == NULL)
        return -1;
    memset(buf, 0, n + 1);
    buf[0] = 'a';
    free(buf);
    return 0;
}

int checked_realloc(char **pp, size_t n)
{
    char *tmp = realloc(*pp, n);
    if (!tmp)
        return -1;
    *pp = tmp;
    tmp[0] = 0;
    return 0;
}
