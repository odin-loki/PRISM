/* Minimal <string.h> for PIR on Windows (LP64 linux-gnu target, no sysroot). */
/* This header is declarations only. */
#ifndef PRISM_PIR_SHIM_STRING_H
#define PRISM_PIR_SHIM_STRING_H
typedef __SIZE_TYPE__ size_t; /* unsigned long (LP64) or unsigned int (ILP32) */
void* memset(void*, int, size_t);
void* memcpy(void*, const void*, size_t);
size_t strlen(const char*);
#endif
