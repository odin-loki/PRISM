/* Code in braces no parsed function owns: the parser says so (PARSE-GAP)
 * instead of skipping it quietly. */
#include <stdlib.h>

#define TEST(name) void test_##name(void)
#define BODY { return 7; }

TEST(alloc) {
    char *p = malloc(4);
    free(p);
    p[0] = 1;
}

int macro_body(void) BODY

int after_macro(int q) {
    return q;
}
