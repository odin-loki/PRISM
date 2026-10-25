/* The copies omit the terminator and nothing writes it afterwards (a
 * store to another array does not count). */
#include <stdio.h>
#include <string.h>

void repeat_sin_unterminated(char *expr, char *other, int n)
{
    int j;
    for (j = 0; j < n; ++j)
        memcpy(expr + j * 4, "sin ", 4);
    other[j * 4] = 0;
}

/* The string is read before the store that would terminate it. */
void read_then_reset(char *buf)
{
    memcpy(buf, "abc", 3);
    puts(buf);
    buf[0] = 0;
}
