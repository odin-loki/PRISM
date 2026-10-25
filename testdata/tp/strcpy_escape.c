/* STR-OFF-BY-ONE is measured on the raw literal: "ab\n" is three bytes. */
#include <string.h>

void strcpy_escape_bad(char *out)
{
    char b[3];
    strcpy(b, "ab\n");
    memcpy(out, b, sizeof b);
}

void strcpy_escape_fits(char *out)
{
    char b[4];
    strcpy(b, "ab\n");
    memcpy(out, b, sizeof b);
}
