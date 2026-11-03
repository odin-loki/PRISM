/* Minimal <threads.h> for PIR LP64 lowering on Windows (tests/conc). */
/*
 * Declarations only, not a concurrency proof.
 * A missing body stays unmodeled rather than success.
 */
#ifndef PRISM_PIR_SHIM_THREADS_H
#define PRISM_PIR_SHIM_THREADS_H

typedef unsigned long thrd_t;
typedef int (*thrd_start_t)(void*);

typedef struct {
    char __size[40];
} mtx_t;

typedef struct {
    char __size[48];
} cnd_t;

enum { mtx_plain = 0, mtx_recursive = 1, mtx_timed = 2 };
enum { thrd_success = 0, thrd_busy = 1, thrd_error = 2, thrd_nomem = 3, thrd_timedout = 4 };

int thrd_create(thrd_t* thr, thrd_start_t func, void* arg);
int thrd_join(thrd_t thr, int* res);
int mtx_init(mtx_t* mutex, int type);
int mtx_lock(mtx_t* mutex);
int mtx_trylock(mtx_t* mutex);
int mtx_unlock(mtx_t* mutex);
void mtx_destroy(mtx_t* mutex);
int cnd_init(cnd_t* cond);
int cnd_signal(cnd_t* cond);
int cnd_broadcast(cnd_t* cond);
int cnd_wait(cnd_t* cond, mtx_t* mtx);
void cnd_destroy(cnd_t* cond);

#endif
