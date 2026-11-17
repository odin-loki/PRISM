// EXPECT: BOUNDED. RCU publish/subscribe with release/acquire: rcu_assign_pointer
// is a release store and rcu_dereference an acquire load (the C11 image of the
// LKMM rule that a pointer published after initialisation is seen initialised).
// Source: Linux kernel memory model (tools/memory-model/Documentation, RCU and
// smp_store_release/smp_load_acquire); McKenney, "Is Parallel Programming Hard"
// sec. 9.5; herd7 C11 MP+rel+acq.
// The published pointer is modelled as the index 1 (0 = NULL): the conc stage does not model address values.
#include <pthread.h>
#include <stdatomic.h>
#include <assert.h>
atomic_int cfg_val;
atomic_int gp;
void *upd(void *p) { (void)p; atomic_store_explicit(&cfg_val, 7, memory_order_relaxed);
    atomic_store_explicit(&gp, 1, memory_order_release); return 0; }
void *rdr(void *p) { (void)p; int q = atomic_load_explicit(&gp, memory_order_acquire);
    if (q == 1) assert(atomic_load_explicit(&cfg_val, memory_order_relaxed) == 7); return 0; }
int main(void) { pthread_t a, b; pthread_create(&a, 0, upd, 0); pthread_create(&b, 0, rdr, 0);
    pthread_join(a, 0); pthread_join(b, 0); return 0; }
