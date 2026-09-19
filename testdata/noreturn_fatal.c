#include <stdlib.h>

void fatal(int c) {
    exit(c);
}

_Noreturn void dead_noreturn(int c) {
    exit(c);
}

void dead_attr(int c) __attribute__((noreturn)) {
    exit(c);
}
