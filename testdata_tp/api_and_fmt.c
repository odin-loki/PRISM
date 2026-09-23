/* A real gets() call still fires next to a string that names it; a real
 * non-literal format still fires next to a literal macro format. */
#include <stdio.h>

#define FMT "%d\n"

void gets_real(void)
{
    char b[64];
    puts("never call gets(b)");
    gets(b);
    puts(b);
}

void fmt_real(const char *user, int x)
{
    printf(FMT, x);
    printf(user);
    printf(UNDEFINED_FMT, x);
}
