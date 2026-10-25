static int bump(int x) {
    return x + 1;
}
int caller(int x) {
    return bump(x);
}
