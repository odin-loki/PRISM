int typeident_bad(int *p) {
    using T = std::type_identity<int>::type;
    T *q = p;
    return q[9];
}
int typeident_ok(int *p) {
    using T = std::type_identity<int>::type;
    T *q = p;
    return q[0];
}
