// PRISM conformance task c23/c23_embed_true.c: expected true (no-oob)
static const unsigned char data[] = {
#embed "c23_embed_data.bin"
};
int c23_embed_true(int i) {
    if (i < 0 || i >= (int)sizeof data) return 0;
    return data[i];
}
