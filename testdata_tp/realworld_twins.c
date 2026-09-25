/* True-positive twins of testdata_fp/realworld_switch.c and
 * realworld_forms.c (docs/EVALUATION.md): each must still fire. */
#include <stdlib.h>

#define TEST(name) void test_##name(void)

int real_fallthrough(int x) {
    int r = 0;
    switch (x) {
    case 1:
        r = 1;
    case 2:
        r += 2;
        break;
    }
    return r;
}

int vla_from_param(int n) {
    char buf[n];
    buf[0] = 1;
    return buf[0];
}

int vla_from_caps_local(int n) {
    int LEN = n;
    char buf[LEN];
    buf[0] = 1;
    return buf[0];
}

TEST(alloc) {
    char *p = malloc(4);
    free(p);
    p[0] = 1;
}

struct node { int type; };

int null_then_deref(struct node *n) {
    if (!n) return n->type;
    if (n == NULL) {
        n->type = 0;
    }
    return 0;
}

int bool_bits(int a, int b, int c, int d)
{
    int r = (a < b) | (c < d);
    if (a == 1 & b == 2)
        return r;
    return 0;
}

int no_return_in_else(int x)
{
#ifdef FAST
    return x;
#else
    x++;
#endif
}
