// PRISM conformance task c23/c23_embed_if_empty_false.c: expected false (no-oob)
static const unsigned char data[] = {
#embed "c23_embed_data.bin" limit(0) if_empty(7)
};
int c23_embed_if_empty_false(int i) {
    if (i < 0 || i > 1) return 0;
    return data[i];
}
