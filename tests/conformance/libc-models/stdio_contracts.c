/* <stdio.h> models (src/prism/pir/models/libc/stdio.c) against C17 7.21.
 * See harness.h. Each function is one contract; `_false` ones must fail.
 * printf/fprintf/sprintf/snprintf are modelled in the translator
 * (src/prism/pir/libc_format.cpp), not in C, and are not covered here. */
#include "harness.h"
#include "../../../src/prism/pir/models/libc/string.c" /* strlen for fopen/fputs/puts */
#include "../../../src/prism/pir/models/libc/stdio.c"

/* 7.21.5.3 / 7.21.5.1: fopen returns NULL or an open stream; fclose closes it. */
int fopen_true(void) {
    char path[2] = {'f', 0}, mode[2] = {'r', 0};
    FILE *f = fopen(path, mode);
    if (!f) return 0;
    (void)fflush(f);
    (void)fclose(f);
    return 0;
}

/* Closing a stream twice. */
int fclose_twice_false(void) {
    char path[2] = {'f', 0}, mode[2] = {'r', 0};
    FILE *f = fopen(path, mode);
    if (!f) return 0;
    fclose(f);
    fclose(f);
    return 0;
}

/* Wrong contract: fopen never fails. */
int fopen_null_false(void) {
    char path[2] = {'f', 0}, mode[2] = {'r', 0};
    FILE *f = fopen(path, mode);
    return fgetc(f);
}

/* 7.21.7.2: fgets reads at most n-1 characters and NUL-terminates. */
int fgets_true(int n) {
    char buf[N];
    char path[2] = {'f', 0}, mode[2] = {'r', 0};
    if (n < 1 || n > N) return 0;
    FILE *f = fopen(path, mode);
    if (!f) return 0;
    char *r = fgets(buf, n, f);
    if (r) {
        assert(r == buf);
        assert(strlen(buf) < (size_t)n);
    }
    fclose(f);
    return 0;
}

/* n larger than the buffer. */
int fgets_oob_false(void) {
    char buf[N];
    char path[2] = {'f', 0}, mode[2] = {'r', 0};
    FILE *f = fopen(path, mode);
    if (!f) return 0;
    (void)fgets(buf, N + 2, f);
    fclose(f);
    return 0;
}

/* 7.21.8.1 / 7.21.8.2: fread/fwrite return at most nmemb. */
int fread_fwrite_true(int nmemb) {
    int a[N];
    char path[2] = {'f', 0}, mode[2] = {'r', 0};
    if (nmemb < 0 || nmemb > N) return 0;
    FILE *f = fopen(path, mode);
    if (!f) return 0;
    size_t k = fread(a, sizeof(int), (size_t)nmemb, f);
    assert(k <= (size_t)nmemb);
    size_t w = fwrite(a, sizeof(int), k, f);
    assert(w <= k);
    fclose(f);
    return 0;
}

/* fread past the end of the buffer. */
int fread_oob_false(int nmemb) {
    int a[N];
    char path[2] = {'f', 0}, mode[2] = {'r', 0};
    if (nmemb < 0 || nmemb > N + 1) return 0;
    FILE *f = fopen(path, mode);
    if (!f) return 0;
    (void)fread(a, sizeof(int), (size_t)nmemb, f);
    fclose(f);
    return 0;
}

/* 7.21.7.1: fgetc returns an unsigned char converted to int, or EOF. */
int fgetc_true(void) {
    char path[2] = {'f', 0}, mode[2] = {'r', 0};
    FILE *f = fopen(path, mode);
    if (!f) return 0;
    int c = fgetc(f);
    assert(c == -1 || (c >= 0 && c <= 255));
    int d = getc(f);
    assert(d == -1 || (d >= 0 && d <= 255));
    fclose(f);
    int e = getchar();
    assert(e == -1 || (e >= 0 && e <= 255));
    return 0;
}

/* Wrong contract: fgetc never returns EOF. */
int fgetc_eof_false(void) {
    int e = getchar();
    assert(e != -1);
    return e;
}

/* 7.21.7.3 / 7.21.7.8 / 7.21.7.9: putc/putchar return the character written. */
int putchar_true(int c) {
    assert(putchar(c) == (int)(unsigned char)c);
    char s[2] = {'x', 0};
    assert(puts(s) >= 0);
    return 0;
}

/* puts on an unterminated array. */
int puts_unterminated_false(void) {
    char s[2] = {'x', 'y'};
    return puts(s);
}
