/* SCALAR loop whose body overflows a signed int. BMC should FAILED. */
int loop_overflow(int x) {
    int i;
    for (i = 0; i < 4; i = i + 1) {
        x = x + x;
    }
    return x;
}
