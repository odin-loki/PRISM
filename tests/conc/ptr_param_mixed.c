// FAILED CONC-DATA-RACE on g: bump() is called with a global (bounded
// memory) and with a local that does not escape; the global call site gets
// a clone of bump that carries the address, so the race on g is found.
#include <pthread.h>
int g;
static void bump(int *p) { *p += 1; }
void *t1(void *p) { (void)p; int local = 0; bump(&local); bump(&g); return 0; }
int main(void) { pthread_t a; pthread_create(&a, 0, t1, 0); bump(&g); pthread_join(a, 0); return 0; }
