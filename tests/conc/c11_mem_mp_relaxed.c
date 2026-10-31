// FAILED CONC-DATA-RACE on data: the flag in a struct is relaxed, so reading
// 1 does not order the plain accesses to data.
#include <pthread.h>
#include <stdatomic.h>
#include <assert.h>
int data;
struct S { int pad; atomic_int flag; } s;
void *t1(void *p) { (void)p; data = 42; atomic_store_explicit(&s.flag, 1, memory_order_relaxed); return 0; }
void *t2(void *p) { (void)p; int seen = 0; if (atomic_load_explicit(&s.flag, memory_order_relaxed) == 1) seen = data; return (void *)(long)seen; }
int main(void) { pthread_t a, b; pthread_create(&a, 0, t1, 0); pthread_create(&b, 0, t2, 0);
    pthread_join(a, 0); pthread_join(b, 0); return 0; }
