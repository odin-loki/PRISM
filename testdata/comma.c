int comma_sum(int x, int y) {
    int a;
    int b;
    a = 0, b = 0;
    a = x, b = y;
    return a - x + b - y;
}
int comma_ovf(int x) {
    int a;
    int b;
    a = x, b = x;
    return a + b;
}
