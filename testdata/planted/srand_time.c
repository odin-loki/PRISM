#include <stdlib.h>
#include <time.h>

int srand_bad(void) {
    srand(time(0));
    return rand();
}

int srand_ok(int seed) {
    srand(seed);
    return rand();
}
