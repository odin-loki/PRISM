// PRISM conformance task c23/c23_enum_fixed_true.c: expected true (no-overflow)
enum color : unsigned char { RED = 1, GREEN = 2, BLUE = 250 };
int c23_enum_fixed_true(int a) {
    if (a < 0 || a > 1000) return 0;
    return a * BLUE;
}
