/* PIR memory tasks: <string.h> through the libc models and the memcpy /
 * memset intrinsics (docs/PIR.md "Library models"). */
#include <string.h>

int strlen_ok(void) {
    const char *s = "hello";
    return (int)strlen(s);
}

int strcpy_bad(void) {
    char d[4];
    strcpy(d, "hello"); /* 6 bytes into 4 */
    return d[0];
}

int strcpy_ok(void) {
    char d[8];
    strcpy(d, "hello");
    return d[4];
}

int unterminated_bad(void) {
    char s[3] = {'a', 'b', 'c'}; /* no NUL */
    return (int)strlen(s);
}

int memcpy_ok(int i) {
    int a[4] = {1, 2, 3, 4}, b[4];
    memcpy(b, a, sizeof a);
    return b[i & 3];
}

int memcpy_bad(int n) {
    char a[8] = {0}, b[4];
    if (n < 0 || n > 8) return 0;
    memcpy(b, a, (size_t)n); /* n > 4 overflows b */
    return 0;
}

int overlap_bad(void) {
    char a[8] = "abcdefg";
    memcpy(a + 1, a, 4); /* overlapping: memmove is required */
    return a[0];
}

int memmove_ok(void) {
    char a[8] = "abcdefg";
    memmove(a + 1, a, 4);
    return a[1];
}

int memset_bad(int n) {
    char a[4];
    if (n < 0 || n > 5) return 0;
    memset(a, 0, (size_t)n); /* n == 5 */
    return 0;
}

int strcmp_ok(void) { return strcmp("abc", "abd") < 0; }

int strcat_bad(void) {
    char d[6] = "abc";
    strcat(d, "def"); /* needs 7 bytes */
    return d[0];
}
