/* Minimal <stdlib.h> for PIR on Windows (LP64 linux-gnu target, no sysroot). */
#ifndef PRISM_PIR_SHIM_STDLIB_H
#define PRISM_PIR_SHIM_STDLIB_H
typedef __SIZE_TYPE__ size_t; /* unsigned long (LP64) or unsigned int (ILP32) */
#define NULL ((void*)0)
void* malloc(size_t);
void free(void*);
void* calloc(size_t, size_t);
void* realloc(void*, size_t);
void abort(void) __attribute__((noreturn));
int atoi(const char*);
#endif
