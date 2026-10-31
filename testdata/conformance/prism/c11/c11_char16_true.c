// PRISM conformance task c11/c11_char16_true.c: expected true (no-oob)
#include <uchar.h>
int c11_char16_true(int x) {
    const char16_t s[] = u"abc";
    if (x < 0 || x > 3) return 0;
    return s[x];
}
