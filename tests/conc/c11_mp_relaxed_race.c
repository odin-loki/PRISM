// FAILED CONC-DATA-RACE: message passing with a relaxed flag. The reader sees
// flag == 1 but no happens-before reaches the plain write of data.
#include <pthread.h>
#include <stdatomic.h>
int data;
atomic_int flag;
void *producer(void *p) { (void)p; data = 42; atomic_store_explicit(&flag, 1, memory_order_relaxed); return 0; }
void *consumer(void *p) {
    (void)p;
    if (atomic_load_explicit(&flag, memory_order_relaxed) == 1) return (void *)(long)data;
    return 0;
}
int main(void) { pthread_t a, b; pthread_create(&a, 0, producer, 0); pthread_create(&b, 0, consumer, 0);
    pthread_join(a, 0); pthread_join(b, 0); return 0; }
