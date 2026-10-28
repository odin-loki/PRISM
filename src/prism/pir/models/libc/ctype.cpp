/* <ctype.h> operational model for the C locale ASCII set only
 * (docs/PIR.md "Library models").
 * EOF is -1. Every is* function returns 0 for EOF. Every to* function
 * returns EOF for EOF. A value that is not EOF and not in a range below
 * is not given a class: is* returns 0 and to* returns that value.
 * This is not a proof of a locale other than the C locale, and it is not PROVED and not CLEAN. */
#include "prism_model.h"

#ifndef EOF
#define EOF (-1)
#endif

extern "C" {

/* Nonzero only for '0'..'9'. A letter is not a digit. */
int isdigit(int c) {
    if (c == EOF) return 0;
    if (c >= '0' && c <= '9') return 1;
    return 0;
}

/* Nonzero only for the C locale standard white-space characters:
 * space, horizontal tab, newline, vertical tab, form feed, carriage return. */
int isspace(int c) {
    if (c == EOF) return 0;
    if (c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r') return 1;
    return 0;
}

/* Nonzero only for 'A'..'Z' and 'a'..'z'. */
int isalpha(int c) {
    if (c == EOF) return 0;
    if (c >= 'A' && c <= 'Z') return 1;
    if (c >= 'a' && c <= 'z') return 1;
    return 0;
}

/* Nonzero only for a digit or a letter. */
int isalnum(int c) {
    if (c == EOF) return 0;
    if (isdigit(c) != 0) return 1;
    if (isalpha(c) != 0) return 1;
    return 0;
}

/* Nonzero only for 'a'..'z'. */
int islower(int c) {
    if (c == EOF) return 0;
    if (c >= 'a' && c <= 'z') return 1;
    return 0;
}

/* Nonzero only for 'A'..'Z'. */
int isupper(int c) {
    if (c == EOF) return 0;
    if (c >= 'A' && c <= 'Z') return 1;
    return 0;
}

/* tolower('A') is 'a'. A non-letter other than EOF is returned unchanged. */
int tolower(int c) {
    if (c == EOF) return EOF;
    if (c >= 'A' && c <= 'Z') return c - 'A' + 'a';
    return c;
}

/* Symmetric to tolower. A non-letter other than EOF is returned unchanged. */
int toupper(int c) {
    if (c == EOF) return EOF;
    if (c >= 'a' && c <= 'z') return c - 'a' + 'A';
    return c;
}

/* Nonzero only for space and horizontal tab. */
int isblank(int c) {
    if (c == EOF) return 0;
    if (c == ' ' || c == '\t') return 1;
    return 0;
}

/* Nonzero only for '0'..'9', 'A'..'F', and 'a'..'f'. */
int isxdigit(int c) {
    if (c == EOF) return 0;
    if (c >= '0' && c <= '9') return 1;
    if (c >= 'A' && c <= 'F') return 1;
    if (c >= 'a' && c <= 'f') return 1;
    return 0;
}

/* Nonzero only for 0x20 through 0x7E. */
int isprint(int c) {
    if (c == EOF) return 0;
    if (c >= 0x20 && c <= 0x7E) return 1;
    return 0;
}

/* Nonzero only for 0x21 through 0x7E. */
int isgraph(int c) {
    if (c == EOF) return 0;
    if (c >= 0x21 && c <= 0x7E) return 1;
    return 0;
}

/* Nonzero only for a graphic character that is not a digit and not a letter. */
int ispunct(int c) {
    if (c == EOF) return 0;
    if (isgraph(c) != 0 && isdigit(c) == 0 && isalpha(c) == 0) return 1;
    return 0;
}

/* Nonzero only for 0x00 through 0x1F and 0x7F. */
int iscntrl(int c) {
    if (c == EOF) return 0;
    if (c >= 0x00 && c <= 0x1F) return 1;
    if (c == 0x7F) return 1;
    return 0;
}

}  // extern "C"
