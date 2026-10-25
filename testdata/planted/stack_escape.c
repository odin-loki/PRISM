int *esc_bad(void) {
    int x;
    x = 0;
    return &x;
}
int *arr_esc_bad(void) {
    int buf[4];
    return buf;
}
int *arr_esc_plus0(void) {
    int buf[4];
    return buf + 0;
}
int *arr_esc_plusi(int i) {
    int buf[4];
    return buf + i;
}
int *arr_esc_plus_rhs(void) {
    int buf[4];
    return 0 + buf;
}
int *arr_esc_paren(void) {
    int buf[4];
    return (buf);
}
int *esc_paren_addr(void) {
    int x;
    x = 0;
    return (&x);
}
int *esc_ok(int *p) {
    return p;
}
