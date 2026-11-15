// EXPECT: BOUNDED. RCU publish/subscribe with release/acquire: rcu_assign_pointer
// is a release store and rcu_dereference an acquire load (the C11 image of the
// LKMM rule that a pointer published after initialisation is seen initialised).
// Source: Linux kernel memory model (tools/memory-model/Documentation, RCU and
// smp_store_release/smp_load_acquire); McKenney, "Is Parallel Programming Hard"
// sec. 9.5; herd7 C11 MP+rel+acq.
#include <pthread.h>
#include <stdatomic.h>
#include <assert.h>
struct cfg { atomic_int val; };
struct cfg c0;
atomic_intptr_t gp;
void *upd(void *p) { (void)p; struct cfg *n = &c0; atomic_store_explicit(&n->val, 7, memory_order_relaxed);
    atomic_store_explicit(&gp, (intptr_t)n, memory_order_release); return 0; }
void *rdr(void *p) { (void)p; struct cfg *q = (struct cfg *)atomic_load_explicit(&gp, memory_order_acquire);
    if (q) assert(atomic_load_explicit(&q->val, memory_order_relaxed) == 7); return 0; }
int main(void) { pthread_t a, b; pthread_create(&a, 0, upd, 0); pthread_create(&b, 0, rdr, 0);
    pthread_join(a, 0); pthread_join(b, 0); return 0; }
