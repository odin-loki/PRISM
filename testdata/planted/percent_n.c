#include <stdio.h>

void percent_n_bad(void) {
    int n;
    printf("%d%n", 42, &n);
    fprintf(stdout, "x%n", &n);
}

void percent_n_ok(void) {
    int x = 1;
    printf("%d", x);
}

void percent_n_escaped_ok(void) {
    printf("100%%n done");
}

void percent_n_snprintf_ok(void) {
    char buf[16];
    snprintf(buf, sizeof buf, "%d", 7);
}
