int bciter_bad(int *p) {
    std::basic_const_iterator<int*> it(p);
    return it[9];
}
int bciter_ok(int *p) {
    std::basic_const_iterator<int*> it(p);
    return it[0];
}
