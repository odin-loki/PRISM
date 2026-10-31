// TRUE (norace, noassert): each thread writes its own field of a shared struct;
// distinct addresses never race. Expected BOUNDED.
#include <assert.h>
#include <pthread.h>

struct pair { int a; int b; } s = {1, 2};

void *wa(void *arg) { (void)arg; s.a = 10; return 0; }
void *wb(void *arg) { (void)arg; s.b = 20; return 0; }

int main(void) {
    pthread_t t1, t2;
    pthread_create(&t1, 0, wa, 0);
    pthread_create(&t2, 0, wb, 0);
    pthread_join(t1, 0);
    pthread_join(t2, 0);
    assert(s.a == 10 && s.b == 20);
    return 0;
}
