// EXPECT: FAILED. The same RCU publish with a relaxed pointer store and load: the
// reader may see the new pointer but not the initialised field. Source: C11
// 5.1.2.4 (no synchronizes-with without release/acquire); Linux memory model
// (rcu_assign_pointer needs smp_store_release, not WRITE_ONCE).
// The published pointer is modelled as the index 1 (0 = NULL): the conc stage does not model address values.
#include <pthread.h>
#include <stdatomic.h>
#include <assert.h>
atomic_int cfg_val;
atomic_int gp;
void *upd(void *p) { (void)p; atomic_store_explicit(&cfg_val, 7, memory_order_relaxed);
    atomic_store_explicit(&gp, 1, memory_order_relaxed); return 0; }
void *rdr(void *p) { (void)p; int q = atomic_load_explicit(&gp, memory_order_relaxed);
    if (q == 1) assert(atomic_load_explicit(&cfg_val, memory_order_relaxed) == 7); return 0; }
int main(void) { pthread_t a, b; pthread_create(&a, 0, upd, 0); pthread_create(&b, 0, rdr, 0);
    pthread_join(a, 0); pthread_join(b, 0); return 0; }
