/* Real-code idioms that null-test p->m before p->m->... (zlib, cJSON,
 * Unity): truthiness in if / && / while / ?:, a *_NULL constant, an assert
 * macro, the else branch of a NULL test, spaced `! p -> m`. */
#include <assert.h>
#include <stddef.h>
#include <string.h>

#define Z_NULL 0
#define TEST_ASSERT_NOT_NULL(p) assert((p) != NULL)

struct link {
    struct link *next;
    struct link *child;
    struct link *prev;
    int v;
    char *text;
};

int if_truth(struct link *a)
{
    if (a->next) {
        return a->next->v;
    }
    return 0;
}

int and_truth(struct link *a)
{
    return a->next && a->next->v;
}

int while_truth(struct link *p)
{
    int s = 0;
    while (p->next) {
        s += p->next->v;
        p = p->next;
    }
    return s;
}

int ternary_truth(struct link *a)
{
    return a->next ? a->next->v : 0;
}

void add_child(struct link *a, struct link *n)
{
    if (a && a->child) {
        a->child->prev = n;
    }
}

int z_null_else(struct link *s)
{
    if (s->next == Z_NULL) {
        return 0;
    } else {
        return s->next->v;
    }
}

int asserted(struct link *item)
{
    TEST_ASSERT_NOT_NULL(item->child->next);
    return item->child->next->v;
}

size_t spaced_not(struct link *o)
{
    if (! o -> text)
        return 0;
    return strlen(o->text);
}

int spaced_test(struct link *a)
{
    if (a -> next == NULL)
        return 0;
    a->next->v = 1;
    return 1;
}
