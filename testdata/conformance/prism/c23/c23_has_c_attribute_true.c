// PRISM conformance task c23/c23_has_c_attribute_true.c: expected true (no-overflow)
#if __has_c_attribute(maybe_unused)
#define MU [[maybe_unused]]
#else
#define MU
#endif
int c23_has_c_attribute_true(short x) { MU int y = 0; return x + 1; }
