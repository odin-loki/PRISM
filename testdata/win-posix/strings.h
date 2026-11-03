/* Windows-only astlint fixture stand-in (see README.md). */
#ifndef PRISM_WIN_POSIX_STRINGS_H
#define PRISM_WIN_POSIX_STRINGS_H
#include <stddef.h>
void bcopy(const void *src, void *dst, size_t n);
void bzero(void *s, size_t n);
int bcmp(const void *a, const void *b, size_t n);
int strcasecmp(const char *a, const char *b);
int strncasecmp(const char *a, const char *b, size_t n);
char *index(const char *s, int c);
char *rindex(const char *s, int c);
#endif
