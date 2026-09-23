/* PRISM operational models of the C library (roadmap 2.6, docs/PIR.md
 * "Library models"). These files are compiled by clang to LLVM IR at run
 * time and linked into the analysed module for every symbol the module
 * declares but does not define. They are ordinary C: every load, store and
 * pointer step inside a model is checked by PIR exactly like user code, so a
 * model's preconditions (the `requires:` comments) are enforced by the
 * memory model, not assumed.
 *
 * The __prism_* functions below are PIR intrinsics (translate.cpp
 * model_intrinsic); they are never defined in C.
 */
#ifndef PRISM_MODEL_H
#define PRISM_MODEL_H

typedef unsigned long size_t;
typedef long ssize_t;
typedef struct PRISM_FILE FILE;

/* allocation kinds (include/prism/pir.hpp MemKind) */
#define PRISM_HEAP 2
#define PRISM_CONST 4
#define PRISM_NEW 5
#define PRISM_NEWARR 6
#define PRISM_EXTERN 7
#define PRISM_FILEK 8
/* initial contents */
#define PRISM_UNINIT 0
#define PRISM_ZERO 1
#define PRISM_HAVOC 2
/* objects stay below 2^47 bytes (pointer encoding, docs/PIR.md) */
#define PRISM_MAX_OBJ (1UL << 47)
#define PRISM_FILE_SIZE 216UL

void *__prism_alloc(size_t size, int kind, int init);
void __prism_free(void *p, int kind);
void __prism_free_check(void *p, int kind);
void __prism_check(int ok, const char *cls, const char *msg);
void __prism_assume(int cond);
/* this execution saw an allocation fail: a later abort() is out-of-memory
 * handling, not a defect (translate.cpp, abort) */
void __prism_alloc_failed(void);
size_t __prism_obj_size(const void *p); /* bytes from p to the end of its object */
void __prism_memcpy(void *d, const void *s, size_t n, int overlap_ok);
void __prism_memset(void *d, int c, size_t n);
void __prism_read_range(const void *p, size_t n);
void __prism_havoc_bytes(void *p, size_t n);
char *__prism_fresh_cstr(int kind);

int __VERIFIER_nondet_int(void);
long __VERIFIER_nondet_long(void);
unsigned long __VERIFIER_nondet_ulong(void);

#endif
