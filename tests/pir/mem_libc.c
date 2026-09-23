/* PIR tasks: libc operational models (abs, getenv, atoi, stdio, printf
 * format checks) and C features (_Generic, VLAs, setjmp). */
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>

int abs_bad(int x) { return abs(x); } /* abs(INT_MIN) */

int abs_ok(int x) {
    if (x == -2147483647 - 1) return 0;
    return abs(x);
}

int getenv_bad(void) {
    const char *h = getenv("HOME");
    return h[0]; /* getenv may return NULL */
}

int getenv_ok(void) {
    const char *h = getenv("HOME");
    return h ? h[0] : 0;
}

int atoi_ok(void) { return atoi("42") != 7; }

int printf_ok(int x) {
    printf("%d %s\n", x, "ok");
    return 0;
}

int printf_n_bad(void) {
    int n = 0;
    printf("abc%n\n", &n); /* %n */
    return n;
}

int printf_args_bad(int x) {
    printf("%d %d\n", x); /* one argument missing */
    return 0;
}

int printf_type_bad(int x) {
    printf("%s\n", x); /* int for %s */
    return 0;
}

int snprintf_ok(int x) {
    char buf[16];
    snprintf(buf, sizeof buf, "%d", x);
    return buf[0] != 0;
}

int sprintf_bad(int x) {
    char buf[4];
    sprintf(buf, "%d", x); /* up to 11 characters and a NUL */
    return buf[0];
}

int fgets_ok(void) {
    char line[32];
    if (!fgets(line, sizeof line, stdin)) return 0;
    return line[0];
}

int fgets_bad(void) {
    char line[8];
    if (!fgets(line, 32, stdin)) return 0; /* size larger than the buffer */
    return line[0];
}

int fopen_bad(void) {
    FILE *f = fopen("/tmp/x", "r");
    int c = fgetc(f); /* fopen may fail */
    fclose(f);
    return c;
}

int fclose_twice_bad(void) {
    FILE *f = fopen("/tmp/x", "r");
    if (!f) return 0;
    fclose(f);
    fclose(f);
    return 1;
}

#define kind(x) _Generic((x), int: 1, long: 2, default: 3)

int generic_ok(int x) { return kind(x) + kind(2L); }

int vla_ok(int n) {
    if (n < 1 || n > 6) return 0;
    int a[n];
    for (int i = 0; i < n; i++) a[i] = i;
    return a[n - 1];
}

int vla_size_bad(int n) {
    if (n > 8) return 0;
    int a[n]; /* n <= 0 is undefined */
    a[0] = 1;
    return a[0];
}

int vla_oob_bad(int n) {
    if (n < 1 || n > 8) return 0;
    int a[n];
    a[n] = 1; /* one past the end */
    return 0;
}

static jmp_buf env;

int setjmp_unenc(int x) {
    if (setjmp(env)) return 1;
    if (x) longjmp(env, 1);
    return 0;
}
