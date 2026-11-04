// FAILED (race on hits): a created thread (not the harness) starts workers in a loop.
#include <pthread.h>
int hits[1];
void *leaf(void *x) { (void)x; hits[0]++; return 0; }
void *boss(void *x) { (void)x; pthread_t t[2]; for (int i = 0; i < 2; ++i) pthread_create(&t[i], 0, leaf, 0);
    for (int i = 0; i < 2; ++i) pthread_join(t[i], 0); return 0; }
int main(void) { pthread_t b; pthread_create(&b, 0, boss, 0); pthread_join(b, 0); return 0; }
