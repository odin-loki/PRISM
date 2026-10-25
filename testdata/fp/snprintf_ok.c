/* Correct: bounded formatting into local buffers, literal formats. */
#include <stdio.h>
#include <string.h>

size_t fmt_id(char *out, size_t n, int id)
{
    char tmp[32];
    if (out == NULL || n == 0)
        return 0;
    snprintf(tmp, sizeof tmp, "id-%d", id);
    strncpy(out, tmp, n - 1);
    out[n - 1] = '\0';
    return strlen(out);
}
