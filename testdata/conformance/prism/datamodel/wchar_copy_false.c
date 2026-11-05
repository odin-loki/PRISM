// PRISM conformance task datamodel/wchar_copy_false.c: expected false (no-oob) for x86_64-unknown-linux-gnu
// wchar_t is 4 bytes on Linux (glibc): copying it into char buf[3] writes past the end
// The same body is the _true task for the other data model (LP64 vs LLP64).
#include <stddef.h>
#include <string.h>
int wchar_copy_false(int i) {
    char buf[3] = {0};
    wchar_t w = (wchar_t)i;
    memcpy(buf, &w, sizeof w);
    return buf[0];
}
