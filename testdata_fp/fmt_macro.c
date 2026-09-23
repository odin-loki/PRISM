/* Correct: every format is a string literal, some spelled as macros. */
#include <stdio.h>

#define FMT "%d\n"
#define PREFIX "value: "
#define LOGFMT "[%s] %s\n"

void fmt_macro(int x, const char *tag, const char *msg)
{
    printf(FMT, x);
    fprintf(stderr, FMT, x);
    printf(PREFIX "%d\n", x);
    printf(LOGFMT, tag, msg);
}

void fmt_percent(char *buf, size_t n, int pct)
{
    printf("100%% done\n");
    snprintf(buf, n, "%d%%", pct);
}
