// PRISM conformance task c23/c23_embed_limit_false.c: expected false (no-oob)
static const unsigned char data[] = {
#embed "c23_embed_data.bin" limit(4)
};
int c23_embed_limit_false(int i) {
    if (i < 0 || i >= 8) return 0;
    return data[i];
}
