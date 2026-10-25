// PRISM conformance task c23/c23_enum_fixed_false.c: expected false (no-overflow)
enum color : unsigned char { RED = 1, GREEN = 2, BLUE = 250 };
int c23_enum_fixed_false(int a) {
    if (a < 0) return 0;
    return a * BLUE;
}
