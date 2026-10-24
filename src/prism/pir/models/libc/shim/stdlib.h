/* Minimal <stdlib.h> for PIR on Windows (LP64 linux-gnu target, no sysroot). */
#ifndef PRISM_PIR_SHIM_STDLIB_H
#define PRISM_PIR_SHIM_STDLIB_H
typedef unsigned long size_t;
#define NULL ((void*)0)
void* malloc(size_t);
void free(void*);
void* calloc(size_t, size_t);
void* realloc(void*, size_t);
void abort(void) __attribute__((noreturn));
int atoi(const char*);
#endif
