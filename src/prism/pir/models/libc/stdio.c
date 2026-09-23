/* <stdio.h> operational models (docs/PIR.md "Library models"). Files and
 * input are external: fopen may fail, reads return unknown bytes, and a
 * FILE object must be open (fclose twice is a double free). printf,
 * fprintf, sprintf and snprintf are modelled in the translator
 * (libc_format.cpp) because their argument lists are variadic. */
#include "prism_model.h"

size_t strlen(const char *s);

FILE *fopen(const char *path, const char *mode) {
    (void)strlen(path);
    (void)strlen(mode);
    if (__VERIFIER_nondet_int()) return 0;
    return (FILE *)__prism_alloc(PRISM_FILE_SIZE, PRISM_FILEK, PRISM_HAVOC);
}

int fclose(FILE *f) {
    __prism_free(f, PRISM_FILEK);
    return __VERIFIER_nondet_int();
}

static void prism_file_ok(FILE *f) {
    __prism_check(f != 0, "PTR-NULL-DEREF", "stdio call on a NULL FILE pointer");
    __prism_read_range(f, PRISM_FILE_SIZE);
}

/* requires: buf writable for n bytes, f an open FILE */
char *fgets(char *buf, int n, FILE *f) {
    prism_file_ok(f);
    if (n <= 0 || __VERIFIER_nondet_int()) return 0;
    size_t len = __VERIFIER_nondet_ulong();
    __prism_assume(len < (size_t)n);
    __prism_havoc_bytes(buf, len);
    buf[len] = 0;
    return buf;
}

size_t fread(void *p, size_t size, size_t nmemb, FILE *f) {
    prism_file_ok(f);
    size_t k = __VERIFIER_nondet_ulong();
    __prism_assume(k <= nmemb);
    size_t t;
    if (__builtin_mul_overflow(k, size, &t)) return 0;
    __prism_havoc_bytes(p, t);
    return k;
}

size_t fwrite(const void *p, size_t size, size_t nmemb, FILE *f) {
    prism_file_ok(f);
    size_t t;
    __prism_check(!__builtin_mul_overflow(size, nmemb, &t), "MEM-OOB-READ", "fwrite size * nmemb overflows");
    __prism_read_range(p, t);
    size_t k = __VERIFIER_nondet_ulong();
    __prism_assume(k <= nmemb);
    return k;
}

int fgetc(FILE *f) {
    prism_file_ok(f);
    int c = __VERIFIER_nondet_int();
    __prism_assume(c >= -1 && c <= 255);
    return c;
}
int getc(FILE *f) { return fgetc(f); }
int getchar(void) {
    int c = __VERIFIER_nondet_int();
    __prism_assume(c >= -1 && c <= 255);
    return c;
}

int fputc(int c, FILE *f) {
    prism_file_ok(f);
    return c & 255;
}
int putc(int c, FILE *f) { return fputc(c, f); }
int putchar(int c) { return c & 255; }

int fputs(const char *s, FILE *f) {
    prism_file_ok(f);
    (void)strlen(s);
    return 0;
}
int puts(const char *s) {
    (void)strlen(s);
    return 0;
}

int fflush(FILE *f) {
    if (f) prism_file_ok(f);
    return 0;
}
