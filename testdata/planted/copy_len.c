#include <string.h>

struct buf {
    char data[64];
    unsigned data_max;
};

void copy_len_bad(struct buf *b, const char *src, unsigned len) {
    memcpy(b->data, src, len);
}

void copy_len_ok(struct buf *b, const char *src, unsigned len) {
    if (len > b->data_max)
        return;
    memcpy(b->data, src, len);
}
