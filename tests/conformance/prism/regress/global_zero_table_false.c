// PRISM conformance task regress/global_zero_table_false.c: expected false (no-div0)
// regression: F8: a large, mostly zero table keeps its exact contents; the
// one entry that makes the divisor wrap to 0 is still found
struct uint3 {
    unsigned int x, y, z;
};
static const struct uint3 table[1024] = {[1000] = {4294967295u, 0, 0}};

unsigned int global_zero_table_false(int i) {
    return 100u / (table[i & 1023].x + 1u);
}
