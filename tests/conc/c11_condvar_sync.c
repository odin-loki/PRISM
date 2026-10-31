// BOUNDED: the condition-variable wait re-acquires the mutex, which acquires
// the signaller's view, so the plain read of data does not race; the ready
// flag is relaxed (switches on the view model).
#include <pthread.h>
#include <stdatomic.h>
#include <assert.h>
pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
int ready, data;
atomic_int ticks;
void *t1(void *p) { (void)p; atomic_fetch_add_explicit(&ticks, 1, memory_order_relaxed);
    pthread_mutex_lock(&m); data = 7; ready = 1; pthread_cond_signal(&cv); pthread_mutex_unlock(&m); return 0; }
void *t2(void *p) { (void)p; pthread_mutex_lock(&m); while (!ready) pthread_cond_wait(&cv, &m);
    pthread_mutex_unlock(&m); assert(data == 7); return 0; }
int main(void) { pthread_t a, b; pthread_create(&a, 0, t1, 0); pthread_create(&b, 0, t2, 0);
    pthread_join(a, 0); pthread_join(b, 0); return 0; }
