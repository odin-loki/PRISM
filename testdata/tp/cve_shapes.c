/* The real CVE code, reduced, that the synthetic twins in
 * realworld_twins.c did not cover. */
#include <assert.h>
#include <stddef.h>
#include <string.h>

#define zmemcpy memcpy

struct node {
    int type;
    char *valuestring;
};

struct head {
    unsigned char *extra;
    unsigned extra_len;
    unsigned extra_max;
};

struct zstate {
    struct head *head;
    unsigned length;
};

struct link {
    struct link *next;
    int v;
};

/* cJSON 1.7.16 cJSON_SetValuestring (CVE-2023-50472): strlen() on the
 * member first, the != NULL test only later. */
char *set_valuestring(struct node *object, const char *valuestring)
{
    if (strlen(valuestring) <= strlen(object->valuestring)) {
        strcpy(object->valuestring, valuestring);
        return object->valuestring;
    }
    if (object->valuestring != NULL) {
        object->valuestring = NULL;
    }
    return NULL;
}

/* zlib 1.2.12 inflate.c:768 (CVE-2022-37434): len is never tested against
 * extra_max, so extra_max - len wraps. */
void inflate_extra(struct zstate *state, const unsigned char *next, unsigned copy)
{
    unsigned len;
    if (state->head != NULL && state->head->extra != NULL) {
        len = state->head->extra_len - state->length;
        zmemcpy(state->head->extra + len, next,
                len + copy > state->head->extra_max ?
                state->head->extra_max - len : copy);
    }
}

/* Spaced member access reaches strlen() unchecked. */
size_t spaced_member(struct node *o)
{
    return strlen(o -> valuestring);
}

/* One unchecked access used on many lines is one row, at the first use. */
int chain_many(struct link *a)
{
    int s = a->next->v;
    s += a->next->v;
    s += a->next->v;
    return s;
}

/* A value assert dereferences a->next itself; it is not a null test. */
int chain_value_assert(struct link *a)
{
    assert(a->next->v == 1);
    return a->next->v;
}
