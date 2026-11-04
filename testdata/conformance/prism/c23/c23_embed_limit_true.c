// PRISM conformance task c23/c23_embed_limit_true.c: expected true (no-oob)
static const unsigned char data[] = {
#embed "c23_embed_data.bin" limit(4) prefix(0, ) suffix(, 0)
};
_Static_assert(sizeof data == 6, "prefix + 4 + suffix");
int c23_embed_limit_true(int i) {
    if (i < 0 || i >= 6) return 0;
    return data[i];
}
