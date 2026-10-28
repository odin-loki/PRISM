/* <stdio.h> operational models (docs/PIR.md "Library models"). Files and
 * input are external: fopen may fail, reads return unknown bytes, and a
 * FILE object must be open (fclose twice is a double free). printf,
 * fprintf, sprintf and snprintf are modelled in the translator
 * (libc_format.cpp) because their argument lists are variadic. */
#include "prism_model.h"

namespace {

/* Output may fail on any call (disk full, closed pipe): every output
 * function also returns EOF (-1) on some path, so code that ignores a write
 * error is reachable (C17 7.21.7.3p3, 7.21.7.4p2, 7.21.5.2p3). */
static int prism_eof_or(int ok) { return __VERIFIER_nondet_int() ? -1 : ok; }
static int prism_nonneg_or_eof(void) {
    int r = __VERIFIER_nondet_int();
    __prism_assume(r >= -1);
    return r;
}

static void prism_file_ok(FILE *f) {
    __prism_check(f != 0, "PTR-NULL-DEREF", "stdio call on a NULL FILE pointer");
    __prism_read_range(f, PRISM_FILE_SIZE);
}

}  // namespace

extern "C" {

size_t strlen(const char *s);

FILE *fopen(const char *path, const char *mode) {
    __prism_check(path != 0, "STR-NULL-ARG", "fopen() called with unchecked pointer parameter path");
    __prism_check(mode != 0, "STR-NULL-ARG", "fopen() called with unchecked pointer parameter mode");
    (void)strlen(path);
    (void)strlen(mode);
    /* A failed fopen returns a null FILE. The success path is the only
     * allocation, so the failure path leaves no FILE object. */
    if (__VERIFIER_nondet_int()) return 0;
    return (FILE *)__prism_alloc(PRISM_FILE_SIZE, PRISM_FILEK, PRISM_HAVOC);
}

/* fclose(NULL) is undefined (glibc crashes); a closed FILE is released. */
int fclose(FILE *f) {
    __prism_check(f != 0, "PTR-NULL-DEREF", "fclose of a NULL FILE pointer");
    __prism_free(f, PRISM_FILEK);
    return prism_eof_or(0);
}

/* requires: when n > 0, buf is non-null and writable for n bytes.
 * n <= 0 writes nothing. f is an open FILE. */
char *fgets(char *buf, int n, FILE *f) {
    prism_file_ok(f);
    if (n <= 0) return 0;
    __prism_check(buf != 0, "STR-NULL-ARG", "fgets() called with unchecked pointer parameter buf");
    if (buf == 0 || __VERIFIER_nondet_int()) return 0;
    size_t len = __VERIFIER_nondet_ulong();
    __prism_assume(len < (size_t)n);
    __prism_havoc_bytes(buf, len);
    buf[len] = 0;
    return buf;
}

size_t fread(void *p, size_t size, size_t nmemb, FILE *f) {
    prism_file_ok(f);
    /* Overflow of size * nmemb is not a short read. */
    size_t nbytes = 0;
    int ovf = __builtin_mul_overflow(size, nmemb, &nbytes);
    if (ovf) {
        __prism_check(0, "MEM-OOB-WRITE", "fread size * nmemb overflows");
        __prism_assume(0);
        return nbytes;
    }
    if (nbytes == 0) return 0;
    __prism_check(p != 0, "STR-NULL-ARG", "fread() called with unchecked pointer parameter p");
    if (p == 0) return 0;
    size_t k = __VERIFIER_nondet_ulong();
    __prism_assume(k <= nmemb);
    if (k != 0) __prism_havoc_bytes(p, k * size);
    return k;
}

size_t fwrite(const void *p, size_t size, size_t nmemb, FILE *f) {
    prism_file_ok(f);
    /* Overflow of size * nmemb is not a short write. */
    size_t t = 0;
    int ovf = __builtin_mul_overflow(size, nmemb, &t);
    if (ovf) {
        __prism_check(0, "MEM-OOB-READ", "fwrite size * nmemb overflows");
        __prism_assume(0);
        return 0;
    }
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

/* fputc/putc/putchar: the character written as unsigned char, or EOF. */
int fputc(int c, FILE *f) {
    prism_file_ok(f);
    return prism_eof_or(c & 255);
}
int putc(int c, FILE *f) { return fputc(c, f); }
int putchar(int c) { return prism_eof_or(c & 255); }

/* fputs/puts: a non-negative value, or EOF; s must be a string. */
int fputs(const char *s, FILE *f) {
    prism_file_ok(f);
    __prism_check(s != 0, "STR-NULL-ARG", "fputs() called with unchecked pointer parameter s");
    (void)strlen(s);
    return prism_nonneg_or_eof();
}
int puts(const char *s) {
    __prism_check(s != 0, "STR-NULL-ARG", "puts() called with unchecked pointer parameter s");
    (void)strlen(s);
    return prism_nonneg_or_eof();
}

/* fflush(NULL) flushes every stream; 0, or EOF on a write error. */
int fflush(FILE *f) {
    if (f) prism_file_ok(f);
    return prism_eof_or(0);
}

/* This is not a proof that the stream is at end of file
 * and not a proof that the stream has an error.
 * feof and ferror are not PROVED and not CLEAN. */
static int prism_file_flag(FILE *f) {
    prism_file_ok(f);
    if (f == 0) {
        __prism_assume(0);
        return 0;
    }
    return __VERIFIER_nondet_int() ? 1 : 0;
}

int feof(FILE *f) { return prism_file_flag(f); }
int ferror(FILE *f) { return prism_file_flag(f); }

}  // extern "C"
