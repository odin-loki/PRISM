// PRISM conformance task datamodel/wchar_copy_true.c: expected true (no-oob) for x86_64-pc-windows-msvc
// wchar_t is 2 bytes on Windows x64: copying it into char buf[3] stays in bounds
// The same body is the _false task for the other data model (LP64 vs LLP64).
#include <stddef.h>
#include <string.h>
int wchar_copy_true(int i) {
    char buf[3] = {0};
    wchar_t w = (wchar_t)i;
    memcpy(buf, &w, sizeof w);
    return buf[0];
}
