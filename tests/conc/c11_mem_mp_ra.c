// BOUNDED: release/acquire message passing through an atomic in an array
// (bounded memory): the acquire load of flags[1] == 1 synchronises.
#include <pthread.h>
#include <stdatomic.h>
#include <assert.h>
int data;
atomic_int flags[2];
void *t1(void *p) { (void)p; data = 42; atomic_store_explicit(&flags[1], 1, memory_order_release); return 0; }
void *t2(void *p) { (void)p; if (atomic_load_explicit(&flags[1], memory_order_acquire) == 1) assert(data == 42); return 0; }
int main(void) { pthread_t a, b; pthread_create(&a, 0, t1, 0); pthread_create(&b, 0, t2, 0);
    pthread_join(a, 0); pthread_join(b, 0); return 0; }
