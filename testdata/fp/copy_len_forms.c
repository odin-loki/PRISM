/* Copies in a file that has *_max bounds: a literal length is a constant,
 * and the zlib 1.2.12.1 fix tests len against extra_max before the
 * zmemcpy whose length is extra_max - len. */
#include <string.h>

#define zmemcpy memcpy

struct head {
    unsigned char *extra;
    unsigned extra_len;
    unsigned extra_max;
};

struct zstate {
    struct head *head;
    unsigned length;
};

void copy_fixed(unsigned char *d, const unsigned char *s)
{
    memcpy(d, s, 16);
}

void copy_extra_fixed(struct zstate *state, const unsigned char *next, unsigned copy)
{
    unsigned len;
    if (state->head != NULL && state->head->extra != NULL &&
        (len = state->head->extra_len - state->length) < state->head->extra_max) {
        zmemcpy(state->head->extra + len, next,
                len + copy > state->head->extra_max ?
                state->head->extra_max - len : copy);
    }
}

void copy_extra_tested(struct zstate *state, const unsigned char *next, unsigned copy,
                       unsigned len)
{
    if (state->head != NULL && len < state->head->extra_max) {
        zmemcpy(state->head->extra + len, next,
                len + copy > state->head->extra_max ?
                state->head->extra_max - len : copy);
    }
}
