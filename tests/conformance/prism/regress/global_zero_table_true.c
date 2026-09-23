// PRISM conformance task regress/global_zero_table_true.c: expected true (no-div0)
// regression: F8: a large (> 4096-byte), mostly zero table keeps its exact
// contents (the zero fill is one memory entry), not arbitrary bytes
struct uint3 {
    unsigned int x, y, z;
};
static const struct uint3 table[1024] = {[1000] = {5, 0, 0}};

unsigned int global_zero_table_true(int i) {
    return 100u / (table[i & 1023].x + 1u);
}
