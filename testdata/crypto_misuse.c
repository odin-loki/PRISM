#include <stdlib.h>

void crypto_key_bad(char *key) {
    int i;
    for (i = 0; i < 8; i++)
        key[i] = rand();
}

void crypto_key_ok(char *key) {
    int i;
    for (i = 0; i < 8; i++)
        key[i] = 0;
}
