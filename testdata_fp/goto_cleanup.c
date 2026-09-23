/* Correct: the single cleanup label frees each buffer once. */
#include <stdlib.h>
#include <string.h>

int goto_cleanup(const char *src)
{
    int rc = -1;
    char *a = NULL;
    char *b = NULL;

    a = malloc(16);
    if (a == NULL)
        goto out;
    b = malloc(16);
    if (b == NULL)
        goto out;
    strncpy(a, src, 15);
    a[15] = '\0';
    memcpy(b, a, 16);
    rc = 0;
out:
    free(b);
    free(a);
    return rc;
}
