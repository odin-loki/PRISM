/* Minimal <pthread.h> for PIR LP64 lowering on Windows (tests/conc). */
#ifndef PRISM_PIR_SHIM_PTHREAD_H
#define PRISM_PIR_SHIM_PTHREAD_H

typedef unsigned long pthread_t;

typedef struct {
    char __size[40];
} pthread_mutex_t;

typedef struct {
    char __size[48];
} pthread_cond_t;

#define PTHREAD_MUTEX_INITIALIZER \
    {                           \
        {0}                     \
    }

int pthread_create(pthread_t* thread, const void* attr, void* (*start)(void*), void* arg);
int pthread_join(pthread_t thread, void** value_ptr);
int pthread_mutex_lock(pthread_mutex_t* mutex);
int pthread_mutex_unlock(pthread_mutex_t* mutex);
int pthread_mutex_init(pthread_mutex_t* mutex, const void* attr);
int pthread_mutex_destroy(pthread_mutex_t* mutex);

#endif
