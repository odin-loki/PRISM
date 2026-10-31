// PRISM conformance task c11/c11_char16_false.c: expected false (no-oob)
#include <uchar.h>
int c11_char16_false(int x) {
    const char16_t s[] = u"abc";
    if (x < 0 || x > 4) return 0;
    return s[x];
}
