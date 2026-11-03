// PRISM conformance task c23/c23_has_c_attribute_false.c: expected false (no-overflow)
#if __has_c_attribute(fallthrough)
int c23_has_c_attribute_false(int x) {
    switch (x & 1) {
    case 0: x += 2; [[fallthrough]];
    default: return x;
    }
}
#endif
