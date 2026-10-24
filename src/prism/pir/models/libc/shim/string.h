/* Minimal <string.h> for PIR on Windows (LP64 linux-gnu target, no sysroot). */
#ifndef PRISM_PIR_SHIM_STRING_H
#define PRISM_PIR_SHIM_STRING_H
typedef unsigned long size_t;
void* memset(void*, int, size_t);
void* memcpy(void*, const void*, size_t);
size_t strlen(const char*);
#endif
