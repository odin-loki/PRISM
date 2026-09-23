// norace / noassert: true. C11 <threads.h>: mtx_t protects the counter.
#include <assert.h>
#include <threads.h>

int counter = 0;
mtx_t m;

int worker(void *arg) {
    (void)arg;
    mtx_lock(&m);
    counter = counter + 1;
    mtx_unlock(&m);
    return 0;
}

int main(void) {
    thrd_t a, b;
    mtx_init(&m, mtx_plain);
    thrd_create(&a, worker, 0);
    thrd_create(&b, worker, 0);
    thrd_join(a, 0);
    thrd_join(b, 0);
    assert(counter == 2);
    return 0;
}
