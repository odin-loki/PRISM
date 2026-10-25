/* Reduced from zlib 1.2.12 (zlib licence: deflate.c, trees.c, crc32.c),
 * docs/EVALUATION.md. K&R definitions behind a lowercase storage macro,
 * a K&R function-pointer parameter, and an #ifdef whose two arms each
 * open a brace: every body here is a function the parser must find. */
#define local static

typedef unsigned char uch;
typedef struct state_s { int level; int pending; } deflate_state;
typedef int block_state;
typedef int once_t;

local void fill_window(s)
    deflate_state *s;
{
    s->pending = 0;
}

local block_state deflate_stored(s, flush)
    deflate_state *s;
    int flush;
{
    return s->level + flush;
}

local void once(state, init)
    once_t *state;
    void (*init)(void);
{
    if (*state == 0) {
        *state = 1;
        init();
    }
}

void flush_block(s, stored_len)
    deflate_state *s;
    unsigned stored_len;
{
#ifdef FORCE_STORED
    if (stored_len > 0) {
#else
    if (stored_len + 4 > 4 && s->level > 0) {
#endif
        s->pending = 1;
    } else {
        s->pending = 2;
    }
}

local unsigned long after_ifdef(s)
    deflate_state *s;
{
    return (unsigned long)s->pending;
}
