/* Windows-only astlint fixture stand-in (see README.md). */
#ifndef PRISM_WIN_POSIX_PTHREAD_H
#define PRISM_WIN_POSIX_PTHREAD_H
#include <stddef.h>
#include <time.h>
typedef unsigned long long pthread_t;
typedef struct { int __opaque[10]; } pthread_mutex_t;
typedef struct { int __opaque[2]; } pthread_mutexattr_t;
typedef struct { int __opaque[12]; } pthread_cond_t;
typedef struct { int __opaque[2]; } pthread_condattr_t;
typedef struct { int __opaque[14]; } pthread_rwlock_t;
typedef struct { int __opaque[2]; } pthread_rwlockattr_t;
typedef struct { int __opaque[8]; } pthread_barrier_t;
typedef struct { int __opaque[2]; } pthread_barrierattr_t;
typedef struct { int __opaque[14]; } pthread_attr_t;
typedef volatile int pthread_spinlock_t;
typedef unsigned pthread_key_t;
typedef int pthread_once_t;
typedef int clockid_t;
#ifndef PRISM_WIN_POSIX_SIGSET_T
#  define PRISM_WIN_POSIX_SIGSET_T
typedef unsigned long long sigset_t;
#endif
#define PTHREAD_MUTEX_INITIALIZER {{0}}
#define PTHREAD_COND_INITIALIZER {{0}}
#define PTHREAD_RWLOCK_INITIALIZER {{0}}
#define PTHREAD_ONCE_INIT 0
#define PTHREAD_CREATE_JOINABLE 0
#define PTHREAD_CREATE_DETACHED 1
#define PTHREAD_BARRIER_SERIAL_THREAD (-1)
#define PTHREAD_PROCESS_PRIVATE 0
#define SIG_BLOCK 0
#define SIG_UNBLOCK 1
#define SIG_SETMASK 2
int pthread_create(pthread_t *t, const pthread_attr_t *a, void *(*fn)(void *), void *arg);
int pthread_join(pthread_t t, void **ret);
int pthread_detach(pthread_t t);
int pthread_cancel(pthread_t t);
int pthread_kill(pthread_t t, int sig);
int pthread_yield(void);
pthread_t pthread_self(void);
int pthread_once(pthread_once_t *o, void (*fn)(void));
int pthread_atfork(void (*prepare)(void), void (*parent)(void), void (*child)(void));
int pthread_sigmask(int how, const sigset_t *set, sigset_t *old);
int pthread_getcpuclockid(pthread_t t, clockid_t *c);
int pthread_attr_init(pthread_attr_t *a);
int pthread_attr_destroy(pthread_attr_t *a);
int pthread_attr_getdetachstate(const pthread_attr_t *a, int *s);
int pthread_attr_setdetachstate(pthread_attr_t *a, int s);
int pthread_attr_getstack(const pthread_attr_t *a, void **p, size_t *n);
int pthread_attr_setstack(pthread_attr_t *a, void *p, size_t n);
int pthread_attr_getstacksize(const pthread_attr_t *a, size_t *n);
int pthread_attr_setstacksize(pthread_attr_t *a, size_t n);
int pthread_mutex_init(pthread_mutex_t *m, const pthread_mutexattr_t *a);
int pthread_mutex_destroy(pthread_mutex_t *m);
int pthread_mutex_lock(pthread_mutex_t *m);
int pthread_mutex_trylock(pthread_mutex_t *m);
int pthread_mutex_unlock(pthread_mutex_t *m);
int pthread_cond_init(pthread_cond_t *c, const pthread_condattr_t *a);
int pthread_cond_destroy(pthread_cond_t *c);
int pthread_cond_wait(pthread_cond_t *c, pthread_mutex_t *m);
int pthread_cond_timedwait(pthread_cond_t *c, pthread_mutex_t *m, const struct timespec *t);
int pthread_cond_signal(pthread_cond_t *c);
int pthread_cond_broadcast(pthread_cond_t *c);
int pthread_rwlock_init(pthread_rwlock_t *l, const pthread_rwlockattr_t *a);
int pthread_rwlock_destroy(pthread_rwlock_t *l);
int pthread_rwlock_rdlock(pthread_rwlock_t *l);
int pthread_rwlock_wrlock(pthread_rwlock_t *l);
int pthread_rwlock_tryrdlock(pthread_rwlock_t *l);
int pthread_rwlock_trywrlock(pthread_rwlock_t *l);
int pthread_rwlock_timedrdlock(pthread_rwlock_t *l, const struct timespec *t);
int pthread_rwlock_timedwrlock(pthread_rwlock_t *l, const struct timespec *t);
int pthread_rwlock_unlock(pthread_rwlock_t *l);
int pthread_spin_init(pthread_spinlock_t *s, int pshared);
int pthread_spin_destroy(pthread_spinlock_t *s);
int pthread_spin_lock(pthread_spinlock_t *s);
int pthread_spin_trylock(pthread_spinlock_t *s);
int pthread_spin_unlock(pthread_spinlock_t *s);
int pthread_barrier_init(pthread_barrier_t *b, const pthread_barrierattr_t *a, unsigned n);
int pthread_barrier_destroy(pthread_barrier_t *b);
int pthread_barrier_wait(pthread_barrier_t *b);
int pthread_key_create(pthread_key_t *k, void (*dtor)(void *));
int pthread_key_delete(pthread_key_t k);
void *pthread_getspecific(pthread_key_t k);
int pthread_setspecific(pthread_key_t k, const void *v);
#endif
