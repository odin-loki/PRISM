/* True-positive twins of testdata_fp/realworld_switch.c and
 * realworld_forms.c (docs/EVALUATION.md): each must still fire. */
#include <stdlib.h>
#include <string.h>

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

struct node { int type; char *valuestring; };
struct link { struct link *prev; struct link *next; };
struct gzhead { unsigned char *extra; unsigned extra_max; };
struct zstate { struct gzhead *head; unsigned char *input; };

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

/* cJSON 1.7.16/1.7.17 cJSON_SetValuestring (CVE-2024-31755): valuestring
 * reaches strlen() unchecked. */
unsigned long set_value_unchecked(struct node *object, const char *valuestring)
{
    if (object->type == 0 || object->type == 3) {
        return 0;
    }
    return strlen(valuestring);
}

/* cJSON 1.7.16 CVE-2023-50472 shape: member field reaches strlen unchecked. */
unsigned long set_value_member(struct node *object, const char *valuestring)
{
    (void)valuestring;
    return strlen(object->valuestring);
}

unsigned long set_value_member_ok(struct node *object, const char *valuestring)
{
    (void)valuestring;
    if (object->valuestring == NULL) {
        return 0;
    }
    return strlen(object->valuestring);
}

/* cJSON 1.7.16 CVE-2023-50471 shape: chained dereference without a null check. */
void insert_list_bad(struct link *after, struct link *newitem)
{
    newitem->prev = after->prev;
    newitem->prev->next = newitem;
}

void insert_list_ok(struct link *after, struct link *newitem)
{
    newitem->prev = after->prev;
    if (newitem->prev == NULL) {
        return;
    }
    newitem->prev->next = newitem;
}

/* zlib 1.2.12 CVE-2022-37434 shape: memcpy length not checked against extra_max. */
void inflate_extra_bad(struct zstate *state, unsigned len)
{
    memcpy(state->head->extra, state->input, len);
}

void inflate_extra_ok(struct zstate *state, unsigned len)
{
    if (len > state->head->extra_max) {
        return;
    }
    memcpy(state->head->extra, state->input, len);
}

int flag_unset(int k) {
    int err;
    if (k > 0) k--;
    if (err) return -1;
    return k;
}
